// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2017-2019 The Swipp developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING.daemon or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "collectionhashing.h"
#include "constraints.h"
#include "init.h"
#include "localization.h"
#include "main.h"
#include "net.h"
#include "rpcserver.h"
#include "smessage.h"
#include "spork.h"
#include "txdb.h"
#include "util.h"

#ifdef ENABLE_WALLET
#include "wallet.h"
#include "walletdb.h"
#endif

#include <errno.h>
#include <limits>
#include <utility>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <openssl/crypto.h>

#ifndef WIN32
#include <signal.h>
#endif

using namespace std;
using namespace boost;

#ifdef ENABLE_WALLET
CWallet* pwalletMain = NULL;
#endif

bool fConfChange;
bool fMinimizeCoinAge;
unsigned int nNodeLifespan;
unsigned int nDerivationMethodIndex;
unsigned int nMinerSleep;
bool fUseFastIndex;
bool fOnlyTor = false;
//enum Checkpoints::CPMode CheckpointsMode;

// Thread management and startup/shutdown:
//
// The network-processing threads are all part of a thread group.
//
// A clean exit happens when StartShutdown() or the SIGTERM
// signal handler sets fRequestShutdown, which triggers
// the DetectShutdownThread(), which interrupts the main thread group.
// DetectShutdownThread() then exits, which causes AppInit() to
// continue (it .joins the shutdown thread).
// Shutdown() is then called to clean up database connections, and stop other
// threads that should only be stopped after the main network-processing
// threads have exited.

volatile bool fRequestShutdown = false;

void StartShutdown()
{
    fRequestShutdown = true;
}

bool ShutdownRequested()
{
    return fRequestShutdown;
}

void Shutdown()
{
    LogPrintf("Shutdown : In progress...\n");

    static CCriticalSection cs_Shutdown;
    TRY_LOCK(cs_Shutdown, lockShutdown);

    if (!lockShutdown)
        return;

    RenameThread("Swipp-shutoff");
    mempool.AddTransactionsUpdated(1);
    StopRPCThreads();
    SecureMsgShutdown();

#ifdef ENABLE_WALLET
    ShutdownRPCMining();

    if (pwalletMain)
        bitdb.Flush(false);
#endif

    StopNode();
    {
        LOCK(cs_main);

#ifdef ENABLE_WALLET
        if (pwalletMain)
            pwalletMain->SetBestChain(CBlockLocator(pindexBest));
#endif
    }

#ifdef ENABLE_WALLET
    if (pwalletMain)
        bitdb.Flush(true);
#endif

    boost::filesystem::remove(GetPidFile());
    UnregisterAllWallets();

#ifdef ENABLE_WALLET
    delete pwalletMain;
    pwalletMain = NULL;
#endif

    LogPrintf("Shutdown : done\n");
}

// Signal handlers are very limited in what they are allowed to do, so:
void HandleSIGTERM(int)
{
    fRequestShutdown = true;
}

void HandleSIGHUP(int)
{
    fReopenDebugLog = true;
}

bool static InitError(const std::string &str)
{
    #warning InitError(str) not returning a proper error code to the OS and should not be used
    fprintf(stderr, "%s\n", str.c_str());
    return false;
}

int static InitError(const std::string &str, int error)
{
    fprintf(stderr, "%s\n", str.c_str());
    return error;
}

bool static InitWarning(const std::string &str)
{
    printf("%s\n", str.c_str());
    return true;
}

bool static Bind(const CService &addr, bool fError = true)
{
    if (IsLimited(addr))
        return false;

    std::string strError;

    if (!BindListenPort(addr, strError))
    {
        if (fError)
            return InitError(strError);

        return false;
    }

    return true;
}

// Core-specific options shared between UI and daemon
std::string HelpMessage()
{
    string strUsage = std::string(_("Options:")) + "\n";
    strUsage += "  -?, --help             " + std::string(_("This help message")) + "\n";
    strUsage += "  -version, --version    " + std::string(_("Show version information")) + "\n";
    strUsage += "  -conf=<file>           " + std::string(_("Specify configuration file (default: swipp.conf)")) + "\n";
    strUsage += "  -pid=<file>            " + std::string(_("Specify pid file (default: swippd.pid)")) + "\n";
    strUsage += "  -datadir=<dir>         " + std::string(_("Specify data directory")) + "\n";
    strUsage += "  -wallet=<dir>          " + std::string(_("Specify wallet file (within data directory)")) + "\n";
    strUsage += "  -dbcache=<n>           " + std::string(_("Set database cache size in megabytes (default: 100)")) + "\n";
    strUsage += "  -dblogsize=<n>         " + std::string(_("Set database disk log size in megabytes (default: 100)")) + "\n";
    strUsage += "  -timeout=<n>           " + std::string(_("Specify connection timeout in milliseconds (default: 5000)")) + "\n";
    strUsage += "  -proxy=<ip:port>       " + std::string(_("Connect through SOCKS5 proxy")) + "\n";
    strUsage += "  -tor=<ip:port>         " + std::string(_("Use proxy to reach tor hidden services (default: same as -proxy)")) + "\n";
    strUsage += "  -dns                   " + std::string(_("Allow DNS lookups for -addnode, -seednode and -connect")) + "\n";
    strUsage += "  -port=<port>           " + std::string(_("Listen for connections on <port> (default: 24055 or testnet: 18065)")) + "\n";
    strUsage += "  -maxconnections=<n>    " + std::string(_("Maintain at most <n> connections to peers (default: 200)")) + "\n";
    strUsage += "  -addnode=<ip>          " + std::string(_("Add a node to connect to and attempt to keep the connection open")) + "\n";
    strUsage += "  -connect=<ip>          " + std::string(_("Connect only to the specified node(s)")) + "\n";
    strUsage += "  -seednode=<ip>         " + std::string(_("Connect to a node to retrieve peer addresses, and disconnect")) + "\n";
    strUsage += "  -externalip=<ip>       " + std::string(_("Specify your own public address")) + "\n";
    strUsage += "  -onlynet=<net>         " + std::string(_("Only connect to nodes in network <net> (IPv4, IPv6 or Tor)")) + "\n";
    strUsage += "  -discover              " + std::string(_("Discover own IP address (default: 1 when listening and no -externalip)")) + "\n";
    strUsage += "  -irc                   " + std::string(_("Find peers using internet relay chat (default: 0)")) + "\n";
    strUsage += "  -listen                " + std::string(_("Accept connections from outside (default: 1 if no -proxy or -connect)")) + "\n";
    strUsage += "  -bind=<addr>           " + std::string(_("Bind to given address. Use [host]:port notation for IPv6")) + "\n";
    strUsage += "  -dnsseed               " + std::string(_("Query for peer addresses via DNS lookup, if low on addresses "
                                                            "(default: 1 unless -connect)")) + "\n";
    strUsage += "  -forcednsseed          " + std::string(_("Always query for peer addresses via DNS lookup (default: 0)")) + "\n";
    strUsage += "  -synctime              " + std::string(_("Sync time with other nodes. Disable if time on your system is precise e.g. "
                                                            "syncing with NTP (default: 1)")) + "\n";
    strUsage += "  -cppolicy              " + std::string(_("Sync checkpoints policy (default: strict)")) + "\n";
    strUsage += "  -banscore=<n>          " + std::string(_("Threshold for disconnecting misbehaving peers (default: 100)")) + "\n";
    strUsage += "  -bantime=<n>           " + std::string(_("Number of seconds to keep misbehaving peers from reconnecting "
                                                            "(default: 86400)")) + "\n";
    strUsage += "  -maxreceivebuffer=<n>  " + std::string(_("Maximum per-connection receive buffer, <n>*1000 bytes (default: 5000)")) + "\n";
    strUsage += "  -maxsendbuffer=<n>     " + std::string(_("Maximum per-connection send buffer, <n>*1000 bytes (default: 1000)")) + "\n";

#ifdef USE_UPNP
#if USE_UPNP
    strUsage += "  -upnp                  " + std::string(_("Use UPnP to map the listening port (default: 1 when listening)")) + "\n";
#else
    strUsage += "  -upnp                  " + std::string(_("Use UPnP to map the listening port (default: 0)")) + "\n";
#endif
#endif

    strUsage += "  -paytxfee=<amt>        " + std::string(_("Fee per KB to add to transactions you send")) + "\n";
    strUsage += "  -mininput=<amt>        " + std::string(_("When creating transactions, ignore inputs with value less than this "
                                                            "(default: 0.01)")) + "\n";
    strUsage += "  -testnet               " + std::string(_("Use the test network")) + "\n";
    strUsage += "  -debug=<category>      " + std::string(_("Output debugging information (default: 0, supplying "
                                                            "<category> is optional)")) + "\n";
    strUsage += "                         " + std::string(_("If <category> is not supplied, output all debugging information.")) + "\n";
    strUsage += "                         " + std::string(_("<category> can be:")) + "\n";
    strUsage += "                         " + std::string("   addrman, alert, db, lock, rand, rpc, selectcoins, mempool, net,\n");
    strUsage += "                         " + std::string("   coinage, coinstake, creation, stakemodifier.\n");
#if !defined(WIN32)
    strUsage += "  -daemon                " + std::string(_("Run in the background as a daemon (default: false)")) + "\n";
#endif
    strUsage += "  -debugbacktrace        " + std::string(_("Output backtrace debugging information, disabled by default")) + "\n";
    strUsage += "  -logtimestamps         " + std::string(_("Prepend debug output with timestamp")) + "\n";
    strUsage += "  -shrinkdebugfile       " + std::string(_("Shrink debug.log file on client startup (default: 1 when no -debug)")) + "\n";
    strUsage += "  -printtoconsole        " + std::string(_("Send trace/debug info to console instead of debug.log file")) + "\n";
    strUsage += "  -regtest               " + std::string(_("Enter regression test mode, which uses a special chain in which "
                                                            "blocks can be solved instantly.\n                         "
                                                            "This is intended for regression testing tools and app development.")) + "\n";
    strUsage += "  -rpcuser=<user>        " + std::string(_("Username for JSON-RPC connections")) + "\n";
    strUsage += "  -rpcpassword=<pw>      " + std::string(_("Password for JSON-RPC connections")) + "\n";
    strUsage += "  -rpcport=<port>        " + std::string(_("Listen for JSON-RPC connections on <port> (default: 35075 or "
                                                            "testnet: 15075)")) + "\n";
    strUsage += "  -rpcallowip=<ip>       " + std::string(_("Allow JSON-RPC connections from specified IP address")) + "\n";

    strUsage += "  -rpcconnect=<ip>       " + std::string(_("Send commands to node running on <ip> (default: 127.0.0.1)")) + "\n";
    strUsage += "  -rpcwait               " + std::string(_("Wait for RPC server to start")) + "\n";

    strUsage += "  -rpcthreads=<n>        " + std::string(_("Set the number of threads to service RPC calls (default: 4)")) + "\n";
    strUsage += "  -blocknotify=<cmd>     " + std::string(_("Execute command when the best block changes (%s in cmd is replaced "
                                                            "by block hash)")) + "\n";
    strUsage += "  -walletnotify=<cmd>    " + std::string(_("Execute command when a wallet transaction changes (%s in cmd is "
                                                            "replaced by TxID)")) + "\n";
    strUsage += "  -confchange            " + std::string(_("Require a confirmations for change (default: 0)")) + "\n";
    strUsage += "  -minimizecoinage       " + std::string(_("Minimize weight consumption (experimental) (default: 0)")) + "\n";
    strUsage += "  -alertnotify=<cmd>     " + std::string(_("Execute command when a relevant alert is received (%s in cmd is "
                                                            "replaced by message)")) + "\n";
    strUsage += "  -upgradewallet         " + std::string(_("Upgrade wallet to latest format")) + "\n";
    strUsage += "  -keypool=<n>           " + std::string(_("Set key pool size to <n> (default: 100)")) + "\n";
    strUsage += "  -rescan                " + std::string(_("Rescan the block chain for missing wallet transactions")) + "\n";
    strUsage += "  -salvagewallet         " + std::string(_("Attempt to recover private keys from a corrupt wallet.dat")) + "\n";
    strUsage += "  -checkblocks=<n>       " + std::string(_("How many blocks to check at startup (default: 500, 0 = all)")) + "\n";
    strUsage += "  -checklevel=<n>        " + std::string(_("How thorough the block verification is (0-6, default: 1)")) + "\n";
    strUsage += "  -loadblock=<file>/web  " + std::string(_("Import blocks from external bootstrap file or *.bsa archive.\n                         "
                                                            "Specify \"web\" to download the latest bootstrap archive from the project website.")) + "\n";
    strUsage += "  -maxorphanblocks=<n>   " + std::string(strprintf(_("Keep at most <n> unconnectable blocks in memory (default: %u)"),
                                                                      DEFAULT_MAX_ORPHAN_BLOCKS)) + "\n";

    strUsage += "\n" + std::string(_("Block creation options:")) + "\n";
    strUsage += "  -blockminsize=<n>       "   + std::string(_("Set minimum block size in bytes (default: 0)")) + "\n";
    strUsage += "  -blockmaxsize=<n>       "   + std::string(_("Set maximum block size in bytes (default: 250000)")) + "\n";
    strUsage += "  -blockprioritysize=<n>  "   + std::string(_("Set maximum size of high-priority/low-fee transactions in bytes "
                                                               "(default: 27000)")) + "\n";

    strUsage += "\n" + std::string(_("SSL options: (see the Bitcoin Wiki for SSL setup instructions)")) + "\n";
    strUsage += "  -rpcssl                                  " + std::string(_("Use OpenSSL (https) for JSON-RPC connections")) + "\n";
    strUsage += "  -rpcsslcertificatechainfile=<file.cert>  " + std::string(_("Server certificate file (default: server.cert)")) + "\n";
    strUsage += "  -rpcsslprivatekeyfile=<file.pem>         " + std::string(_("Server private key (default: server.pem)")) + "\n";
    strUsage += "  -rpcsslciphers=<ciphers>                 " + std::string(_("Acceptable ciphers (default: TLSv1.2"
                                                                              "+HIGH:TLSv1+HIGH:!SSLv2:!aNULL:!eNULL:!3DES:@STRENGTH)")) + "\n";

    strUsage += "\n" + std::string(_("Masternode options:")) + "\n";
    strUsage += "  -masternode=<n>         " + std::string(_("Enable the client to act as a masternode (0-1, default: 0)")) + "\n";
    strUsage += "  -mnconf=<file>          " + std::string(_("Specify masternode configuration file (default: masternode.conf)")) + "\n";
    strUsage += "  -masternodeprivkey=<n>  " + std::string(_("Set the masternode private key")) + "\n";
    strUsage += "  -masternodeaddr=<n>     " + std::string(_("Set external address:port to get to this masternode "
                                                             "(example: address:port)")) + "\n";

    strUsage += "\n" + std::string(_("Darksend options:")) + "\n";
    strUsage += "  -enabledarksend=<n>        " + std::string(_("Enable use of automated darksend for funds stored in this wallet "
                                                                "(0-1, default: 0)")) + "\n";
    strUsage += "  -darksendrounds=<n>        " + std::string(_("Use N separate masternodes to anonymize funds  (2-8, default: 2)")) + "\n";
    strUsage += "  -anonymizeSwippamount=<n>  " + std::string(_("Keep N Swipp anonymized (default: 0)")) + "\n";
    strUsage += "  -liquidityprovider=<n>     " + std::string(_("Provide liquidity to Darksend by infrequently mixing coins on a "
                                                                "continual basis\n                             "
                                                                "(0-100, default: 0, 1=very frequent, high fees, "
                                                                "100=very infrequent, low fees)")) + "\n";
    strUsage += "  -litemode=<n>              " + std::string(_("Disable all Masternode and Darksend related functionality "
                                                                "(0-1, default: 0)")) + "\n";

    strUsage += "\n" + std::string(_("InstantX options:")) + "\n";
    strUsage += "  -enableinstantx=<n>  " + std::string(_("Enable instantx, show confirmations for locked transactions "
                                                          "(bool, default: true)")) + "\n";
    strUsage += "  -instantxdepth=<n>   " + std::string(_("Show N confirmations for a successfully locked transaction "
                                                          "(0-9999, default: 1)")) + "\n";

    strUsage += "\n" + std::string(_("Secure messaging options:")) + "\n";
    strUsage += "  -nosmsg         " + std::string(_("Disable secure messaging.")) + "\n";
    strUsage += "  -debugsmsg      " + std::string(_("Log extra debug messages.")) + "\n";
    strUsage += "  -smsgscanchain  " + std::string(_("Scan the block chain for public key addresses on startup.")) + "\n";

    strUsage += "\n" + std::string(_("Network control options:")) + "\n";
    strUsage += "  --masternodepaymentskey=<n>  " + std::string(_("Set the private control key for the masternode payments master.")) + "\n";
    strUsage += "  --sporkkey=<n>               " + std::string(_("Set the private control key for the spork manager.")) + "\n";
    strUsage += "                               " + std::string(_("For the test network, the default private WIF keys are;")) + "\n";
    strUsage += "                               " + std::string(_("[Masternode payments master] ")) +
                                                    std::string("92kyYbFWnSaCCaMXo8bcbHM2ooCaNZpJbjRUsQS9XDFLX4Ka4AJ\n");
    strUsage += "                               " + std::string(_("[Sporks] ")) + "92cgFu5pK9rwiu9FwFucy2fk3PeCjGQn1i6egB5A5A7vRyXR6j2\n";
    strUsage += "                               " + std::string(_("For the public network, the private keys are controlled by the "
                                                                  "Swipp team.")) + "\n";
    return strUsage;
}

bool InitSanityCheck(void)
{
    if(!ECC_InitSanityCheck())
    {
        InitError("OpenSSL appears to lack support for elliptic curve cryptography. For more "
                  "information, visit https://en.bitcoin.it/wiki/OpenSSL_and_EC_Libraries");
        return false;
    }

    // TODO: Remaining sanity checks, see #4081
    return true;
}

static inline void InitializeCollections()
{
    setStakeSeen.set_empty_key(std::make_pair(COutPoint(), std::numeric_limits<unsigned int>::max()));
    setStakeSeenOrphan.set_empty_key(std::make_pair(COutPoint(), std::numeric_limits<unsigned int>::max()));
    setStakeSeenOrphan.set_deleted_key(std::make_pair(COutPoint(), std::numeric_limits<unsigned int>::max() - 1));
}

int AppInit2(boost::thread_group& threadGroup)
{
    InitializeCollections();

#ifdef _MSC_VER
    // Turn off Microsoft heap dump noise
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, CreateFileA("NUL", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0));
#endif

#if _MSC_VER >= 1400
    // Disable confusing "helpful" text message on abort, Ctrl-C
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

#ifdef WIN32
    // Enable Data Execution Prevention (DEP)
    // Minimum supported OS versions: WinXP SP3, WinVista >= SP1, Win Server 2008
    // A failure is non-critical and needs no further attention!
#ifndef PROCESS_DEP_ENABLE
    // We define this here, because GCCs winbase.h limits this to _WIN32_WINNT >= 0x0601 (Windows 7),
    // which is not correct. Can be removed, when GCCs winbase.h is fixed!
#define PROCESS_DEP_ENABLE 0x00000001
#endif
    typedef BOOL (WINAPI *PSETPROCDEPPOL)(DWORD);
    PSETPROCDEPPOL setProcDEPPol = (PSETPROCDEPPOL)GetProcAddress(GetModuleHandleA("Kernel32.dll"), "SetProcessDEPPolicy");

    if (setProcDEPPol != NULL)
        setProcDEPPol(PROCESS_DEP_ENABLE);
#endif

#ifndef WIN32
    umask(077);

    // Clean shutdown on SIGTERM
    struct sigaction sa;
    sa.sa_handler = HandleSIGTERM;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    // Reopen debug.log on SIGHUP
    struct sigaction sa_hup;
    sa_hup.sa_handler = HandleSIGHUP;
    sigemptyset(&sa_hup.sa_mask);
    sa_hup.sa_flags = 0;
    sigaction(SIGHUP, &sa_hup, NULL);
#endif

    nNodeLifespan = GetArg("-addrlifespan", 7);
    fUseFastIndex = GetBoolArg("-fastindex", true);
    nMinerSleep = GetArg("-minersleep", 500);

    /* CheckpointsMode = Checkpoints::STRICT;
    std::string strCpMode = GetArg("-cppolicy", "strict");

    if(strCpMode == "strict")
        CheckpointsMode = Checkpoints::STRICT;

    if(strCpMode == "advisory")
        CheckpointsMode = Checkpoints::ADVISORY;

    if(strCpMode == "permissive")
        CheckpointsMode = Checkpoints::PERMISSIVE; */

    nDerivationMethodIndex = 0;

    if (!SelectParamsFromCommandLine())
        return InitError("Invalid combination of -testnet and -regtest.");

    if (TestNet())
        SoftSetBoolArg("-irc", true);

    if (mapArgs.count("-bind"))
    {
        // When specifying an explicit binding address, you want to listen on it even when -connect or -proxy is specified
        if (SoftSetBoolArg("-listen", true))
            LogPrintf("AppInit2 : parameter interaction: -bind set -> setting -listen=1\n");
    }

    if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0)
    {
        // When only connecting to trusted nodes, do not seed via DNS, or listen by default
        if (SoftSetBoolArg("-dnsseed", false))
            LogPrintf("AppInit2 : parameter interaction: -connect set -> setting -dnsseed=0\n");

        if (SoftSetBoolArg("-listen", false))
            LogPrintf("AppInit2 : parameter interaction: -connect set -> setting -listen=0\n");
    }

    if (mapArgs.count("-proxy"))
    {
        // To protect privacy, do not listen by default if a default proxy server is specified
        if (SoftSetBoolArg("-listen", false))
            LogPrintf("AppInit2 : parameter interaction: -proxy set -> setting -listen=0\n");

        // To protect privacy, do not discover addresses by default
        if (SoftSetBoolArg("-discover", false))
            LogPrintf("AppInit2 : parameter interaction: -proxy set -> setting -discover=0\n");
    }

    if (!GetBoolArg("-listen", true))
    {
        // Do not map ports or try to retrieve public IP when not listening (pointless)
        if (SoftSetBoolArg("-upnp", false))
            LogPrintf("AppInit2 : parameter interaction: -listen=0 -> setting -upnp=0\n");

        if (SoftSetBoolArg("-discover", false))
            LogPrintf("AppInit2 : parameter interaction: -listen=0 -> setting -discover=0\n");
    }

    if (mapArgs.count("-externalip"))
    {
        // If an explicit public IP is specified, do not try to find others
        if (SoftSetBoolArg("-discover", false))
            LogPrintf("AppInit2 : parameter interaction: -externalip set -> setting -discover=0\n");
    }

    if (GetBoolArg("-salvagewallet", false))
    {
        // Rewrite just private keys: rescan to find transactions
        if (SoftSetBoolArg("-rescan", true))
            LogPrintf("AppInit2 : parameter interaction: -salvagewallet=1 -> setting -rescan=1\n");
    }

    fDebug = !mapMultiArgs["-debug"].empty();

    // Special-case: if -debug=0/-nodebug is set, turn off debugging messages
    const vector<string>& categories = mapMultiArgs["-debug"];

    if (GetBoolArg("-nodebug", false) || find(categories.begin(), categories.end(), string("0")) != categories.end())
        fDebug = false;

    if(fDebug)
        fDebugSmsg = true;
    else
        fDebugSmsg = GetBoolArg("-debugsmsg", false);

    fDebugBacktrace = GetBoolArg("-debugbacktrace", false);
    fNoSmsg = GetBoolArg("-nosmsg", false);

    // Check for -debugnet (deprecated)
    if (GetBoolArg("-debugnet", false))
        InitWarning(_("Warning: Deprecated argument -debugnet ignored, use -debug=net"));

    // Check for -socks - as this is a privacy risk to continue, exit here
    if (mapArgs.count("-socks"))
        return InitError(_("Error: Unsupported argument -socks found. Setting SOCKS version isn't possible anymore, "
                           "only SOCKS5 proxies are supported."));

    fPrintToConsole = GetBoolArg("-printtoconsole", false);
    fLogTimestamps = GetBoolArg("-logtimestamps", false);

#ifdef ENABLE_WALLET
    bool fDisableWallet = GetBoolArg("-disablewallet", false);
#endif

    if (mapArgs.count("-timeout"))
    {
        int nNewTimeout = GetArg("-timeout", 5000);

        if (nNewTimeout > 0 && nNewTimeout < 600000)
            nConnectTimeout = nNewTimeout;
    }

#ifdef ENABLE_WALLET
    if (mapArgs.count("-paytxfee"))
    {
        if (!ParseMoney(mapArgs["-paytxfee"], nTransactionFee))
            return InitError(strprintf(_("Invalid amount for -paytxfee=<amount>: '%s'"), mapArgs["-paytxfee"]));

        if (nTransactionFee > 0.25 * COIN)
            InitWarning(_("Warning: -paytxfee is set very high! This is the transaction fee you will "
                          "pay if you send a transaction."));
    }
#endif

    fConfChange = GetBoolArg("-confchange", false);
    fMinimizeCoinAge = GetBoolArg("-minimizecoinage", false);

#ifdef ENABLE_WALLET
    if (mapArgs.count("-mininput"))
    {
        if (!ParseMoney(mapArgs["-mininput"], nMinimumInputValue))
            return InitError(strprintf(_("Invalid amount for -mininput=<amount>: '%s'"), mapArgs["-mininput"]));
    }
#endif

    // Sanity check
    if (!InitSanityCheck())
        return InitError(_("Initialization sanity check failed. Swipp is shutting down."));

    std::string strDataDir = GetDataDir().string();

#ifdef ENABLE_WALLET
    std::string strWalletFileName = GetArg("-wallet", "wallet.dat");

    // strWalletFileName must be a plain filename without a directory
    if (strWalletFileName != boost::filesystem::basename(strWalletFileName) + boost::filesystem::extension(strWalletFileName))
        return InitError(strprintf(_("Wallet %s resides outside data directory %s."), strWalletFileName, strDataDir));
#endif

    // Make sure only a single Bitcoin process is using the data directory.
    boost::filesystem::path pathLockFile = GetDataDir() / ".lock";
    FILE* file = fopen(pathLockFile.string().c_str(), "a"); // Empty lock file; created if it doesn't exist.

    if (file)
        fclose(file);

    static boost::interprocess::file_lock lock(pathLockFile.string().c_str());

    if (!lock.try_lock())
        return InitError(strprintf(_("Cannot obtain a lock on data directory %s. "
                                     "Swipp is probably already running."), strDataDir), EEXIST);

    if (GetBoolArg("-shrinkdebugfile", !fDebug))
        ShrinkDebugFile();

    LogPrintf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
    LogPrintf("Swipp version %s (%s)\n", FormatFullVersion(), CLIENT_DATE);
    LogPrintf("Using OpenSSL version %s\n", SSLeay_version(SSLEAY_VERSION));

    if (!fLogTimestamps)
        LogPrintf("Startup time: %s\n", DateTimeStrFormat("%x %H:%M:%S", GetTime()));

    LogPrintf("Default data directory %s\n", GetDefaultDataDir().string());
    LogPrintf("Used data directory %s\n", strDataDir);

    std::ostringstream strErrors;

    if (mapArgs.count("-masternodepaymentskey"))
    {
        if (!masternodePayments.SetPrivKey(GetArg("-masternodepaymentskey", "")))
            return InitError(_("Unable to sign masternode payment winner, wrong key?"));
    }

    if (mapArgs.count("-sporkkey"))
    {
        if (!sporkManager.SetPrivKey(GetArg("-sporkkey", "")))
            return InitError(_("Unable to sign spork message, wrong key?"));
    }

    if (fDaemon)
        fprintf(stdout, _("Swipp daemon starting\n"));

    int64_t nStart;

#ifdef ENABLE_WALLET
    if (!fDisableWallet)
    {
        LogPrintf(_("Verifying database integrity...\n"));

        if (!bitdb.Open(GetDataDir()))
        {
            // Try moving the database env out of the way
            boost::filesystem::path pathDatabase = GetDataDir() / "database";
            boost::filesystem::path pathDatabaseBak = GetDataDir() / strprintf("database.%d.bak", GetTime());

            try
            {
                boost::filesystem::rename(pathDatabase, pathDatabaseBak);
                LogPrintf("Moved old %s to %s. Retrying.\n", pathDatabase.string(), pathDatabaseBak.string());
            }
            catch(boost::filesystem::filesystem_error &error)
            {
                 // Failure is ok (well, not really, but it's not worse than what we started with)
            }

            // Try again
            if (!bitdb.Open(GetDataDir()))
            {
                // If it still fails, it probably means we can't even create the database env
                string msg = strprintf(_("Error initializing wallet database environment %s!"), strDataDir);
                return InitError(msg);
            }
        }

        if (GetBoolArg("-salvagewallet", false))
        {
            // Recover readable keypairs:
            if (!CWalletDB::Recover(bitdb, strWalletFileName, true))
                return false;
        }

        if (filesystem::exists(GetDataDir() / strWalletFileName))
        {
            CDBEnv::VerifyResult r = bitdb.Verify(strWalletFileName, CWalletDB::Recover);

            if (r == CDBEnv::RECOVER_OK)
            {
                string msg = strprintf(_("Warning: wallet.dat corrupt, data salvaged!"
                                         " Original wallet.dat saved as wallet.{timestamp}.bak in %s; if"
                                         " your balance or transactions are incorrect you should"
                                         " restore from a backup."), strDataDir);
                InitWarning(msg);
            }

            if (r == CDBEnv::RECOVER_FAIL)
                return InitError(_("wallet.dat corrupt, salvage failed"));
        }

    }
#endif

    RegisterNodeSignals(GetNodeSignals());

    if (mapArgs.count("-onlynet"))
    {
        std::set<enum Network> nets;

        BOOST_FOREACH(std::string snet, mapMultiArgs["-onlynet"])
        {
            enum Network net = ParseNetwork(snet);

            if(net == NET_TOR)
                fOnlyTor = true;

            if (net == NET_UNROUTABLE)
                return InitError(strprintf(_("Unknown network specified in -onlynet: '%s'"), snet));

            nets.insert(net);
        }

        for (int n = 0; n < NET_MAX; n++)
        {
            enum Network net = (enum Network) n;

            if (!nets.count(net))
                SetLimited(net);
        }
    }

    CService addrProxy;
    bool fProxy = false;

    if (mapArgs.count("-proxy"))
    {
        addrProxy = CService(mapArgs["-proxy"], 9050);

        if (!addrProxy.IsValid())
            return InitError(strprintf(_("Invalid -proxy address: '%s'"), mapArgs["-proxy"]));

        if (!IsLimited(NET_IPV4))
            SetProxy(NET_IPV4, addrProxy);

        if (!IsLimited(NET_IPV6))
            SetProxy(NET_IPV6, addrProxy);

        SetNameProxy(addrProxy);
        fProxy = true;
    }

    // -tor can override normal proxy, -notor disables tor entirely
    if (!(mapArgs.count("-tor") && mapArgs["-tor"] == "0") && (fProxy || mapArgs.count("-tor")))
    {
        CService addrOnion;

        if (!mapArgs.count("-tor"))
            addrOnion = addrProxy;
        else
            addrOnion = CService(mapArgs["-tor"], 9050);

        if (!addrOnion.IsValid())
            return InitError(strprintf(_("Invalid -tor address: '%s'"), mapArgs["-tor"]));

        SetProxy(NET_TOR, addrOnion);
        SetReachable(NET_TOR);
    }

    // See Step 2: parameter interactions for more information about these
    fNoListen = !GetBoolArg("-listen", true);
    fDiscover = GetBoolArg("-discover", true);
    fNameLookup = GetBoolArg("-dns", true);
    bool fBound = false;

    if (!fNoListen)
    {
        std::string strError;

        if (mapArgs.count("-bind"))
        {
            BOOST_FOREACH(std::string strBind, mapMultiArgs["-bind"])
            {
                CService addrBind;

                if (!Lookup(strBind.c_str(), addrBind, GetListenPort(), false))
                    return InitError(strprintf(_("Cannot resolve -bind address: '%s'"), strBind));

                fBound |= Bind(addrBind);
            }
        }
        else
        {
            struct in_addr inaddr_any;
            inaddr_any.s_addr = INADDR_ANY;

            if (!IsLimited(NET_IPV6))
                fBound |= Bind(CService(in6addr_any, GetListenPort()), false);

            if (!IsLimited(NET_IPV4))
                fBound |= Bind(CService(inaddr_any, GetListenPort()), !fBound);
        }

        if (!fBound)
            return InitError(_("Failed to listen on any port. Use -listen=0 if you want this."));
    }

    if (mapArgs.count("-externalip"))
    {
        BOOST_FOREACH(string strAddr, mapMultiArgs["-externalip"])
        {
            CService addrLocal(strAddr, GetListenPort(), fNameLookup);

            if (!addrLocal.IsValid())
                return InitError(strprintf(_("Cannot resolve -externalip address: '%s'"), strAddr));

            AddLocal(CService(strAddr, GetListenPort(), fNameLookup), LOCAL_MANUAL);
        }
    }

#ifdef ENABLE_WALLET
    if (mapArgs.count("-reservebalance")) // ppcoin: reserve balance amount
    {
        if (!ParseMoney(mapArgs["-reservebalance"], nReserveBalance))
        {
            InitError(_("Invalid amount for -reservebalance=<amount>"));
            return false;
        }
    }
#endif

    BOOST_FOREACH(string strDest, mapMultiArgs["-seednode"])
        AddOneShot(strDest);

    if (GetBoolArg("-loadblockindextest", false))
    {
        CTxDB txdb("r");
        txdb.LoadBlockIndex();
        PrintBlockTree();
        return false;
    }

    LogPrintf(_("Loading block index...\n"));
    nStart = GetTimeMillis();

    if (!LoadBlockIndex())
        return InitError(_("Error loading block database"));

    if (fRequestShutdown)
    {
        LogPrintf("Shutdown requested. Exiting.\n");
        return false;
    }

    LogPrintf(" block index %15dms\n", GetTimeMillis() - nStart);

    if (GetBoolArg("-printblockindex", false) || GetBoolArg("-printblocktree", false))
    {
        PrintBlockTree();
        return false;
    }

    if (mapArgs.count("-printblock"))
    {
        string strMatch = mapArgs["-printblock"];
        int nFound = 0;

        for (auto mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
        {
            uint256 hash = (*mi).first;

            if (strncmp(hash.ToString().c_str(), strMatch.c_str(), strMatch.size()) == 0)
            {
                CBlockIndex* pindex = (*mi).second;
                CBlock block;
                block.ReadFromDisk(pindex);
                block.BuildMerkleTree();
                LogPrintf("%s\n", block.ToString());
                nFound++;
            }
        }

        if (nFound == 0)
            LogPrintf("No blocks matching %s were found\n", strMatch);

        return false;
    }

#ifdef ENABLE_WALLET
    if (fDisableWallet)
    {
        pwalletMain = NULL;
        LogPrintf("Wallet disabled!\n");
    }
    else
    {
        LogPrintf(_("Loading wallet...\n"));
        nStart = GetTimeMillis();
        bool fFirstRun = true;
        pwalletMain = new CWallet(strWalletFileName);
        DBErrors nLoadWalletRet = pwalletMain->LoadWallet(fFirstRun);

        if (nLoadWalletRet != DB_LOAD_OK)
        {
            if (nLoadWalletRet == DB_CORRUPT)
                strErrors << _("Error loading wallet.dat: Wallet corrupted") << "\n";
            else if (nLoadWalletRet == DB_NONCRITICAL_ERROR)
            {
                string msg(_("Warning: error reading wallet.dat! All keys read correctly, but transaction data"
                             " or address book entries might be missing or incorrect."));
                InitWarning(msg);
            }
            else if (nLoadWalletRet == DB_TOO_NEW)
                strErrors << _("Error loading wallet.dat: Wallet requires newer version of Swipp") << "\n";
            else if (nLoadWalletRet == DB_NEED_REWRITE)
            {
                strErrors << _("Wallet needed to be rewritten: restart Swipp to complete") << "\n";
                LogPrintf("%s", strErrors.str());
                return InitError(strErrors.str());
            }
            else
                strErrors << _("Error loading wallet.dat") << "\n";
        }

        if (GetBoolArg("-upgradewallet", fFirstRun))
        {
            int nMaxVersion = GetArg("-upgradewallet", 0);

            if (nMaxVersion == 0) // the -upgradewallet without argument case
            {
                LogPrintf("Performing wallet upgrade to %i\n", FEATURE_LATEST);
                nMaxVersion = CLIENT_VERSION;
                pwalletMain->SetMinVersion(FEATURE_LATEST); // permanently upgrade the wallet immediately
            }
            else
                LogPrintf("Allowing wallet upgrade up to %i\n", nMaxVersion);

            if (nMaxVersion < pwalletMain->GetVersion())
                strErrors << _("Cannot downgrade wallet") << "\n";

            pwalletMain->SetMaxVersion(nMaxVersion);
        }

        if (fFirstRun)
        {
            // Create new keyUser and set as default key
            RandAddSeedPerfmon();
            CPubKey newDefaultKey;

            if (pwalletMain->GetKeyFromPool(newDefaultKey))
            {
                pwalletMain->SetDefaultKey(newDefaultKey);

                if (!pwalletMain->SetAddressBookName(pwalletMain->vchDefaultKey.GetID(), ""))
                    strErrors << _("Cannot write default address") << "\n";
            }

            pwalletMain->SetBestChain(CBlockLocator(pindexBest));
        }

        LogPrintf("%s", strErrors.str());
        LogPrintf(" wallet      %15dms\n", GetTimeMillis() - nStart);

        RegisterWallet(pwalletMain);
        CBlockIndex *pindexRescan = pindexBest;

        if (GetBoolArg("-rescan", false))
            pindexRescan = pindexGenesisBlock;
        else
        {
            CWalletDB walletdb(strWalletFileName);
            CBlockLocator locator;

            if (walletdb.ReadBestBlock(locator))
                pindexRescan = locator.GetBlockIndex();
            else
                pindexRescan = pindexGenesisBlock;
        }

        if (pindexBest != pindexRescan && pindexBest && pindexRescan && pindexBest->nHeight > pindexRescan->nHeight)
        {
            LogPrintf(_("Rescanning...\n"));
            LogPrintf("Rescanning last %i blocks (from block %i)...\n",
                      pindexBest->nHeight - pindexRescan->nHeight, pindexRescan->nHeight);

            nStart = GetTimeMillis();
            pwalletMain->ScanForWalletTransactions(pindexRescan, true);

            LogPrintf(" rescan      %15dms\n", GetTimeMillis() - nStart);
            pwalletMain->SetBestChain(CBlockLocator(pindexBest));
            nWalletDBUpdated++;
        }
    }

#else
    LogPrintf("No wallet compiled in!\n");
#endif

    std::vector<std::string> arguments;

    if (mapArgs.count("-loadblock"))
    {
        BOOST_FOREACH(string arg, mapMultiArgs["-loadblock"])
            arguments.push_back(arg);
    }

    threadGroup.create_thread(boost::bind(&ThreadImport, arguments));
    LogPrintf(_("Loading addresses...\n"));
    nStart = GetTimeMillis();

    {
        CAddrDB adb;

        if (!adb.Read(addrman))
            LogPrintf("Invalid or missing peers.dat; recreating\n");
    }

    LogPrintf("Loaded %i addresses from peers.dat  %dms\n",
           addrman.size(), GetTimeMillis() - nStart);
    
    SecureMsgStart(fNoSmsg, GetBoolArg("-smsgscanchain", false));

    if (!CheckDiskSpace())
    {
        StartShutdown();
        return false;
    }

    if (!strErrors.str().empty())
        return InitError(strErrors.str());

    fMasterNode = GetBoolArg("-masternode", false);

    if(fMasterNode)
    {
        LogPrintf("IS DARKSEND MASTER NODE\n");
        strMasterNodeAddr = GetArg("-masternodeaddr", "");
        LogPrintf(" addr %s\n", strMasterNodeAddr.c_str());

        if(!strMasterNodeAddr.empty())
        {
            CService addrTest = CService(strMasterNodeAddr);

            if (!addrTest.IsValid())
                return InitError("Invalid -masternodeaddr address: " + strMasterNodeAddr);
        }

        strMasterNodePrivKey = GetArg("-masternodeprivkey", "");

        if(!strMasterNodePrivKey.empty())
        {
            std::string errorMessage;
            CKey key;
            CPubKey pubkey;

            if(!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, key, pubkey))
                return InitError(_("Invalid masternodeprivkey. Please see documenation."));

            activeMasternode.pubKeyMasternode = pubkey;
        }
        else
            return InitError(_("You must specify a masternodeprivkey in the configuration. Please see documentation for help."));
    }

    fEnableDarksend = GetBoolArg("-enabledarksend", false);
    nDarksendRounds = GetArg("-darksendrounds", 2);

    if(nDarksendRounds > 16)
        nDarksendRounds = 16;

    if(nDarksendRounds < 1)
        nDarksendRounds = 1;

    nLiquidityProvider = GetArg("-liquidityprovider", 0); // 0-100

    if(nLiquidityProvider != 0)
    {
        darkSendPool.SetMinBlockSpacing(std::min(nLiquidityProvider,100)*15);
        fEnableDarksend = true;
        nDarksendRounds = 99999;
    }

    nAnonymizeSwippAmount = GetArg("-anonymizeSwippamount", 0);

    if(nAnonymizeSwippAmount > 999999)
        nAnonymizeSwippAmount = 999999;

    if(nAnonymizeSwippAmount < 2)
        nAnonymizeSwippAmount = 2;

    bool fEnableInstantX = GetBoolArg("-enableinstantx", true);

    if(fEnableInstantX)
    {
        nInstantXDepth = GetArg("-instantxdepth", 5);

        if(nInstantXDepth > 60)
            nInstantXDepth = 60;

        if(nInstantXDepth < 0)
            nAnonymizeSwippAmount = 0;
    }
    else
        nInstantXDepth = 0;

    // Lite mode disables all Masternode and Darksend related functionality
    fLiteMode = GetBoolArg("-litemode", false);

    if(fMasterNode && fLiteMode)
        return InitError("You can not start a masternode in litemode");

    LogPrintf("fLiteMode %d\n", fLiteMode);
    LogPrintf("nInstantXDepth %d\n", nInstantXDepth);
    LogPrintf("Darksend rounds %d\n", nDarksendRounds);
    LogPrintf("Anonymize Swipp Amount %d\n", nAnonymizeSwippAmount);

    /* 
       Denominations
       A note about convertability. Within Darksend pools, each denomination
       is convertable to another.
       For example:
       1Swipp+1000 == (.1Swipp+100)*10
       10Swipp+10000 == (1Swipp+1000)*10
    */
    darkSendDenominations.push_back( (100000 * COIN) + 100000000 );    
    darkSendDenominations.push_back( (10000  * COIN) + 10000000 );
    darkSendDenominations.push_back( (1000   * COIN) + 1000000 );
    darkSendDenominations.push_back( (100    * COIN) + 100000 );
    darkSendDenominations.push_back( (10     * COIN) + 10000 );
    darkSendDenominations.push_back( (1      * COIN) + 1000 );
    darkSendDenominations.push_back( (.1     * COIN) + 100 );

    /* Disabled till we need them
    darkSendDenominations.push_back( (.01      * COIN)+10 );
    darkSendDenominations.push_back( (.001     * COIN)+1 ); */

    darkSendPool.InitCollateralAddress();
    threadGroup.create_thread(boost::bind(&ThreadCheckDarkSendPool));
    RandAddSeedPerfmon();

    // Reindex addresses found in blockchain
    if(GetBoolArg("-reindexaddr", false))
    {
        LogPrintf(_("Rebuilding address index...\n"));
        CBlockIndex *pblockAddrIndex = pindexBest;
        CTxDB txdbAddr("rw");

        while(pblockAddrIndex)
        {
            LogPrintf("Rebuilding address index, block %d\n", pblockAddrIndex->nHeight);
            bool ReadFromDisk(const CBlockIndex* pindex, bool fReadTransactions=true);
            CBlock pblockAddr;

            if(pblockAddr.ReadFromDisk(pblockAddrIndex, true))
                pblockAddr.RebuildAddressIndex(txdbAddr);

            pblockAddrIndex = pblockAddrIndex->pprev;
        }
    }

    LogPrintf("mapBlockIndex.size() = %u\n", mapBlockIndex.size());
    LogPrintf("nBestHeight = %d\n", nBestHeight);

#ifdef ENABLE_WALLET
    LogPrintf("setKeyPool.size() = %u\n",      pwalletMain ? pwalletMain->setKeyPool.size() : 0);
    LogPrintf("mapWallet.size() = %u\n",       pwalletMain ? pwalletMain->mapWallet.size() : 0);
    LogPrintf("mapAddressBook.size() = %u\n",  pwalletMain ? pwalletMain->mapAddressBook.size() : 0);
#endif

    StartNode(threadGroup);

#ifdef ENABLE_WALLET
    // InitRPCMining is needed here so getwork/getblocktemplate in the GUI debug console works properly.
    InitRPCMining();
#endif

    StartRPCThreads();

#ifdef ENABLE_WALLET
    // Mine proof-of-stake blocks in the background
    if (!GetBoolArg("-staking", true))
        LogPrintf("Staking disabled\n");
    else if (pwalletMain)
        threadGroup.create_thread(boost::bind(&ThreadStakeMiner, pwalletMain));
#endif

    LogPrintf(_("Done loading\n"));

#ifdef ENABLE_WALLET
    if (pwalletMain)
    {
        // Add wallet transactions that aren't already in a block to mapTransactions
        pwalletMain->ReacceptWalletTransactions();

        // Run a thread to flush wallet periodically
        threadGroup.create_thread(boost::bind(&ThreadFlushWalletDB, boost::ref(pwalletMain->strWalletFile)));
    }
#endif

    return !fRequestShutdown;
}
