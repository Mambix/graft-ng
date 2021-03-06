#include "server.h"
#include "backtrace.h"
#include <boost/program_options.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include "requests.h"
#include "requestdefines.h"
#include "requests/sendsupernodeannouncerequest.h"
#include "rta/supernode.h"
#include "rta/fullsupernodelist.h"

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "supernode.server"


namespace consts {
   static const char * DATA_PATH = "supernode/data";
   static const char * STAKE_WALLET_PATH = "stake-wallet";
   static const char * WATCHONLY_WALLET_PATH = "stake-wallet";
   static const size_t DEFAULT_STAKE_WALLET_REFRESH_INTERFAL_MS = 5 * 1000;
}

namespace tools {
    // TODO: make it crossplatform
    std::string getHomeDir()
    {
        return std::string(getenv("HOME"));
    }
}

namespace graft {

static std::function<void (int sig_num)> int_handler, term_handler, hup_handler;

static void signal_handler_shutdown(int sig_num)
{
    if(int_handler) int_handler(sig_num);
}

static void signal_handler_terminate(int sig_num)
{
    if(term_handler) term_handler(sig_num);
}

static void signal_handler_restart(int sig_num)
{
    if(hup_handler) hup_handler(sig_num);
}

void GraftServer::setHttpRouters(HttpConnectionManager& httpcm)
{
    Router dapi_router("/dapi/v2.0");
    auto http_test = [](const Router::vars_t&, const Input&, Context&, Output&)->Status
    {
        std::cout << "blah-blah" << std::endl;
        return Status::Ok;
    };
    // Router::Handler3 h3_test1(http_test, nullptr, nullptr);

    // dapi_router.addRoute("/test", METHOD_GET, h3_test1);
    // httpcm.addRouter(dapi_router);

    // Router http_router;
    graft::registerRTARequests(dapi_router);
    httpcm.addRouter(dapi_router);

    Router forward_router;
    graft::registerForwardRequests(forward_router);
    httpcm.addRouter(forward_router);

    Router health_router;
    graft::registerHealthcheckRequests(health_router);
    httpcm.addRouter(health_router);

    Router debug_router;
    graft::registerDebugRequests(debug_router);
    httpcm.addRouter(debug_router);


}

void GraftServer::setCoapRouters(CoapConnectionManager& coapcm)
{
    Router coap_router("/coap");
    auto coap_test = [](const Router::vars_t&, const Input&, Context&, Output&)->Status
    {
        std::cout << "blah" << std::endl;
        return Status::Ok;
    };
    Router::Handler3 h3_test(coap_test, nullptr, nullptr);

    coap_router.addRoute("/test", METHOD_GET, h3_test);
    coap_router.addRoute("/test1", METHOD_GET, h3_test);
    coap_router.addRoute("/test2", METHOD_GET, h3_test);

    coapcm.addRouter(coap_router);
}

void GraftServer::initGlobalContext()
{
//  TODO: why context intialized second time here?
    graft::Context ctx(m_looper->getGcm());
    const ConfigOpts& copts = m_looper->getCopts();
//  copts is empty here

//    ctx.global["testnet"] = copts.testnet;
//    ctx.global["watchonly_wallets_path"] = copts.watchonly_wallets_path;
//    ctx.global["cryptonode_rpc_address"] = copts.cryptonode_rpc_address;
}

bool GraftServer::init(int argc, const char** argv)
{
    ConfigOpts configOpts;
    bool res = initConfigOption(argc, argv, configOpts);
    if(!res) return false;

    assert(!m_looper);
    m_looper = std::make_unique<Looper>(configOpts);
    assert(m_looper);

    intiConnectionManagers();

    prepareDataDirAndSupernodes();


    addGlobalCtxCleaner();

    startSupernodePeriodicTasks();

    for(auto& cm : m_conManagers)
    {
        cm->enableRouting();
        checkRoutes(*cm);
        cm->bind(*m_looper);
    }

    initGlobalContext();

    return true;
}

void GraftServer::serve()
{
    LOG_PRINT_L0("Starting server on: [http] " << getCopts().http_address << ", [coap] " << getCopts().coap_address);

    m_looper->serve();
}

bool GraftServer::run(int argc, const char** argv)
{
    initSignals();

    for(bool run = true; run;)
    {
        run = false;
        GraftServer gs;

        if(!gs.init(argc, argv)) return false;
        argc = 1;

        //shutdown
        int_handler = [&gs](int sig_num)
        {
            LOG_PRINT_L0("Stopping server");
            gs.stop();
        };

        //terminate
        term_handler = [&gs](int sig_num)
        {
            LOG_PRINT_L0("Force stopping server");
            gs.stop(true);
        };

        //restart
        hup_handler = [&gs,&run](int sig_num)
        {
            LOG_PRINT_L0("Restarting server");
            run = true;
            gs.stop();
        };

        gs.serve();
    }
    return true;
}

void GraftServer::initSignals()
{
    struct sigaction sa;

    sigemptyset(&sa.sa_mask);

    sa.sa_sigaction = graft_bt_sighandler;
    sa.sa_flags = SA_SIGINFO;

    ::sigaction(SIGSEGV, &sa, NULL);

    sa.sa_sigaction = NULL;
    sa.sa_flags = 0;
    sa.sa_handler = signal_handler_shutdown;
    ::sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = signal_handler_terminate;
    ::sigaction(SIGTERM, &sa, NULL);

    sa.sa_handler = signal_handler_restart;
    ::sigaction(SIGHUP, &sa, NULL);
}

namespace details
{

std::string trim_comments(std::string s)
{
    //remove ;; tail
    std::size_t pos = s.find(";;");
    if(pos != std::string::npos)
    {
      s = s.substr(0,pos);
    }
    boost::trim_right(s);
    return s;
}

namespace po = boost::program_options;

void init_log(const boost::property_tree::ptree& config, const po::variables_map& vm)
{
    std::string log_level = "3";
    bool log_console = true;
    std::string log_filename;
    std::string log_format;

    //from config
    const boost::property_tree::ptree& log_conf = config.get_child("logging");
    boost::optional<std::string> level  = log_conf.get_optional<std::string>("loglevel");
    if(level) log_level = trim_comments( level.get() );
    boost::optional<std::string> log_file  = log_conf.get_optional<std::string>("logfile");
    if(log_file) log_filename = trim_comments( log_file.get() );
    boost::optional<bool> log_to_console  = log_conf.get_optional<bool>("console");
    if(log_to_console) log_console = log_to_console.get();
    boost::optional<std::string> log_fmt  = log_conf.get_optional<std::string>("log-format");
    if(log_fmt) log_format = trim_comments( log_fmt.get() );

    //override from cmdline
    if (vm.count("log-level")) log_level = vm["log-level"].as<std::string>();
    if (vm.count("log-file")) log_filename = vm["log-file"].as<std::string>();
    if (vm.count("log-console")) log_console = vm["log-console"].as<bool>();
    if (vm.count("log-format")) log_format = vm["log-format"].as<std::string>();

    // default log format (we need to explicitly apply it here, otherwise full path to a file will be logged  with monero default format)
    static const char * DEFAULT_LOG_FORMAT = "%datetime{%Y-%M-%d %H:%m:%s.%g}\t%thread\t%level\t%logger\t%rfile:%line\t%msg";
    if(log_format.empty()) log_format = DEFAULT_LOG_FORMAT;

#ifdef ELPP_SYSLOG
    if(log_filename == "syslog")
    {
        INITIALIZE_SYSLOG("graft_server");
        mlog_syslog = true;
        mlog_configure("", false, log_format.empty()? nullptr : log_format.c_str());
    }
    else
#endif
    {
        mlog_configure(log_filename, log_console, log_format.empty()? nullptr : log_format.c_str());
    }

    mlog_set_log(log_level.c_str());
}

} //namespace details

void usage(const boost::program_options::options_description& desc)
{
    std::string sigmsg = "Supported signals:\n"
            "  INT  - Shutdown server gracefully closing all pending tasks.\n"
            "  TEMP - Shutdown server even if there are pending tasks.\n"
            "  HUP  - Restart server with updated configuration parameters.\n";

    std::cout << desc << "\n" << sigmsg << "\n";
}

bool GraftServer::initConfigOption(int argc, const char** argv, ConfigOpts& configOpts)
{
    namespace po = boost::program_options;

    std::string config_filename;
    po::variables_map vm;

    {
        po::options_description desc("Allowed options");
        desc.add_options()
                ("help", "produce help message")
                ("config-file", po::value<std::string>(), "config filename (config.ini by default)")
                ("log-level", po::value<std::string>(), "log-level. (3 by default), e.g. --log-level=2,supernode.task:INFO,supernode.server:DEBUG")
                ("log-console", po::value<bool>(), "log to console. 1 or true or 0 or false. (true by default)")
#ifdef ELPP_SYSLOG
                ("log-file", po::value<std::string>(), "log file; set it to syslog if you want use the syslog instead")
#else
                ("log-file", po::value<std::string>(), "log file")
#endif
                ("log-format", po::value<std::string>(), "e.g. %datetime{%Y-%M-%d %H:%m:%s.%g} %level	%logger	%rfile	%msg");

        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);

        if (vm.count("help")) {
            usage(desc);
            return false;
        }

        if (vm.count("config-file")) {
            config_filename = vm["config-file"].as<std::string>();
        }
    }

    // load config
    boost::property_tree::ptree config;
    namespace fs = boost::filesystem;

    if (config_filename.empty()) {
        fs::path selfpath = argv[0];
        selfpath = selfpath.remove_filename();
        config_filename  = (selfpath / "config.ini").string();
    }

    boost::property_tree::ini_parser::read_ini(config_filename, config);
    // now we have only following parameters
    // [server]
    //  address <IP>:<PORT>
    //  workers-count <integer>
    //  worker-queue-len <integer>
    //  stake-wallet <string> # stake wallet filename (no path)
    // [cryptonode]
    //  rpc-address <IP>:<PORT>
    //  p2p-address <IP>:<PORT> #maybe
    // [upstream]
    //  uri_name=uri_value #pairs for uri substitution
    //
    // data directory structure
    //        .
    //        ├── stake-wallet
    //        │   ├── stake-wallet
    //        │   ├── stake-wallet.address.txt
    //        │   └── stake-wallet.keys
    //        └── watch-only-wallets
    //            ├── supernode_tier1_1
    //            ├── supernode_tier1_1.address.txt
    //            ├── supernode_tier1_1.keys
    //            ................................
    //            ├── supernode_tier1_2
    //            ├── supernode_tier1_2.address.txt
    //            └── supernode_tier1_2.keys

    details::init_log(config, vm);


    const boost::property_tree::ptree& server_conf = config.get_child("server");
    configOpts.http_address = server_conf.get<std::string>("http-address");
    configOpts.coap_address = server_conf.get<std::string>("coap-address");
    configOpts.timer_poll_interval_ms = server_conf.get<int>("timer-poll-interval-ms");
    configOpts.http_connection_timeout = server_conf.get<double>("http-connection-timeout");
    configOpts.workers_count = server_conf.get<int>("workers-count");
    configOpts.worker_queue_len = server_conf.get<int>("worker-queue-len");
    configOpts.upstream_request_timeout = server_conf.get<double>("upstream-request-timeout");
    configOpts.data_dir = server_conf.get<std::string>("data-dir");
    configOpts.lru_timeout_ms = server_conf.get<int>("lru-timeout-ms");
    configOpts.testnet = server_conf.get<bool>("testnet", false);
    configOpts.stake_wallet_name = server_conf.get<string>("stake-wallet-name", "stake-wallet");
    configOpts.stake_wallet_refresh_interval_ms = server_conf.get<size_t>("stake-wallet-refresh-interval-ms",
                                                                      consts::DEFAULT_STAKE_WALLET_REFRESH_INTERFAL_MS);
    if (configOpts.data_dir.empty()) {
        boost::filesystem::path p = boost::filesystem::absolute(tools::getHomeDir());
        p /= ".graft/";
        p /= consts::DATA_PATH;
        configOpts.data_dir = p.string();
    }

    const boost::property_tree::ptree& cryptonode_conf = config.get_child("cryptonode");
    configOpts.cryptonode_rpc_address = cryptonode_conf.get<std::string>("rpc-address");

    const boost::property_tree::ptree& log_conf = config.get_child("logging");
    boost::optional<int> log_trunc_to_size  = log_conf.get_optional<int>("trunc-to-size");
    configOpts.log_trunc_to_size = (log_trunc_to_size)? log_trunc_to_size.get() : -1;

    const boost::property_tree::ptree& uri_subst_conf = config.get_child("upstream");
    graft::OutHttp::uri_substitutions.clear();
    std::for_each(uri_subst_conf.begin(), uri_subst_conf.end(),[&uri_subst_conf](auto it)
    {
        std::string name(it.first);
        std::string val(uri_subst_conf.get<std::string>(name));
        graft::OutHttp::uri_substitutions.insert({std::move(name), std::move(val)});
    });

    return true;
}

void GraftServer::prepareDataDirAndSupernodes()
{
    // create data directory if not exists
    boost::filesystem::path data_path(getCopts().data_dir);
    boost::filesystem::path stake_wallet_path = data_path / "stake-wallet";
    boost::filesystem::path watchonly_wallets_path = data_path / "watch-only-wallets";

    if (!boost::filesystem::exists(data_path)) {
        boost::system::error_code ec;
        if (!boost::filesystem::create_directories(data_path, ec)) {
            throw std::runtime_error(ec.message());
        }

        if (!boost::filesystem::create_directories(stake_wallet_path, ec)) {
            throw std::runtime_error(ec.message());
        }

        if (!boost::filesystem::create_directories(watchonly_wallets_path, ec)) {
            throw std::runtime_error(ec.message());
        }
    }



    getCopts().watchonly_wallets_path = watchonly_wallets_path.string();

    MINFO("data path: " << data_path.string());
    MINFO("stake wallet path: " << stake_wallet_path.string());

    // create supernode instance and put it into global context
    graft::SupernodePtr supernode = boost::make_shared<graft::Supernode>(
                    (stake_wallet_path / getCopts().stake_wallet_name).string(),
                    "", // TODO
                    getCopts().cryptonode_rpc_address,
                    getCopts().testnet
                    );

    supernode->setNetworkAddress(getCopts().http_address + "/dapi/v2.0");

    // create fullsupernode list instance and put it into global context
    graft::FullSupernodeListPtr fsl = boost::make_shared<graft::FullSupernodeList>(
                getCopts().cryptonode_rpc_address, getCopts().testnet);
    size_t found_wallets = 0;
    MINFO("loading supernodes wallets from: " << watchonly_wallets_path.string());
    size_t loaded_wallets = fsl->loadFromDirThreaded(watchonly_wallets_path.string(), found_wallets);

    if (found_wallets != loaded_wallets) {
        LOG_ERROR("found wallets: " << found_wallets << ", loaded wallets: " << loaded_wallets);
    }
    LOG_PRINT_L0("supernode list loaded");

    // add our supernode as well, it wont be added from announce;
    fsl->add(supernode);

    //put fsl into global context
    assert(m_looper);
    graft::Context ctx(m_looper->getGcm());
    ctx.global["supernode"] = supernode;
    ctx.global[CONTEXT_KEY_FULLSUPERNODELIST] = fsl;
    ctx.global["testnet"] = getCopts().testnet;
    ctx.global["watchonly_wallets_path"] = getCopts().watchonly_wallets_path;
    ctx.global["cryptonode_rpc_address"] = getCopts().cryptonode_rpc_address;
}

void GraftServer::intiConnectionManagers()
{
    auto httpcm = std::make_unique<graft::HttpConnectionManager>();
    setHttpRouters(*httpcm);
    m_conManagers.emplace_back(std::move(httpcm));

    auto coapcm = std::make_unique<graft::CoapConnectionManager>();
    setCoapRouters(*coapcm);
    m_conManagers.emplace_back(std::move(coapcm));
}

void GraftServer::addGlobalCtxCleaner()
{
    auto cleaner = [](const graft::Router::vars_t& vars, const graft::Input& input, graft::Context& ctx, graft::Output& output)->graft::Status
    {
        graft::Context::GlobalFriend::cleanup(ctx.global);
        return graft::Status::Ok;
    };
    m_looper->addPeriodicTask(
                graft::Router::Handler3(nullptr, cleaner, nullptr),
                std::chrono::milliseconds(m_looper->getCopts().lru_timeout_ms)
                );
}

void GraftServer::startSupernodePeriodicTasks()
{
    // update supernode every interval_ms

    if (getCopts().stake_wallet_refresh_interval_ms > 0) {
        size_t initial_interval_ms = 1000;
        assert(m_looper);
        m_looper->addPeriodicTask(
                    graft::Router::Handler3(nullptr, sendAnnounce, nullptr),
                    std::chrono::milliseconds(getCopts().stake_wallet_refresh_interval_ms),
                    std::chrono::milliseconds(initial_interval_ms)
                    );
    }
}

void GraftServer::checkRoutes(graft::ConnectionManager& cm)
{//check conflicts in routes
    std::string s = cm.dbgCheckConflictRoutes();
    if(!s.empty())
    {
        std::cout << std::endl << "==> " << cm.getName() << " manager.dbgDumpRouters()" << std::endl;
        std::cout << cm.dbgDumpRouters();

        //if you really need dump of r3tree uncomment two following lines
        //std::cout << std::endl << std::endl << "==> manager.dbgDumpR3Tree()" << std::endl;
        //manager.dbgDumpR3Tree();

        throw std::runtime_error("Routes conflict found:" + s);
    }
}

}//namespace graft
