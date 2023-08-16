#include <cstdlib>
#include <future>
#include <mutex>
#include <filesystem>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include "crow.h"
#include "error.hpp"
#include "json.hpp"
#include "file.hpp"

using namespace std::literals::chrono_literals;
using json = nlohmann::json;

#define CONFIG_FILE "stride.config.json"

int main(int argc, char** argv)
{
    crow::SimpleApp app;

    std::string addr = "localhost", serve, build, watch;
    const std::string& origin_dir = std::filesystem::current_path();
    uint16_t port = 8000;
    bool allow_log = true;
    std::vector<std::string> ignore;

    std::mutex mtx;
    std::unordered_set<crow::websocket::connection*> users;

    if (std::filesystem::exists(CONFIG_FILE))
    {
        json config = json::parse(read_file(CONFIG_FILE));

        addr = config["address"];
        port = config["port"];
        serve = config["serve"];
        build = config["build"];
        watch = config["watch"];
        allow_log = !config["nolog"];
        ignore = config["ignore"];
    }

    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];

        if (arg == "h" || arg == "help")
        {
            std::cout <<
            "commands:\n"
            "\thelp (h)                              prints this message\n"
            "\tinit (i)                              creates config file in current directory\n"
            "options:\n"
            "\t--address <ADDRESS>    (-a, --addr)   sets server address\n"
            "\t--port    <PORT>       (-p)           sets server port\n"
            "\t--serve   <DIRECTORY>  (-s)           sets directory to serve\n"
            "\t--build   <COMMAND>    (-b)           sets build command\n"
            "\t--watch   <<DIRECTORY> (-w)           sets directory to watch for changes in\n"
            "\t--nolog                (-n)           suppresses logging\n";
            return 0;
        }
        else if (arg == "i" || arg == "init")
        {
            write_file(CONFIG_FILE, 
            "{\n"
            "\t\"address\": \"localhost\",\n"
            "\t\"port\": 8000,\n"
            "\t\"serve\": \"\",\n"
            "\t\"build\": \"\",\n"
            "\t\"watch\": \"\",\n"
            "\t\"nolog\": false,\n"
            "\t\"ignore\": []\n"
            "}"
            );

            return 0;
        }
        else if (arg == "-a" || arg == "--addr" || arg == "--address")
        {
            addr = argv[i+1];
            i++;
        }
        else if (arg == "-p" || arg == "--port")
        {
            port = std::stoi(argv[i+1]);
            i++;
        }
        else if (arg == "-s" || arg == "--serve")
        {
            serve = argv[i+1];
            i++;
        }
        else if (arg == "-b" || arg == "--build")
        {
            build = argv[i+1];
            i++;
        }
        else if (arg == "-w" || arg == "--watch")
        {
            watch = argv[i+1];
            i++;
        }
        else if (arg == "-n" || arg == "--nolog")
        {
            allow_log = false;
        }
        else
        {
            error("Unknown parameter: '"+arg+'\'');
            return 1;
        }
    }

    if (addr == "localhost")
        addr = "0.0.0.0";

    if (watch.empty())
        watch = origin_dir;
    else
        watch = std::filesystem::absolute(watch);

    if (!serve.empty())
    {
        if (!std::filesystem::exists(serve))
            error("can not find '"+serve+"'");

        std::filesystem::current_path(serve);
    }

    CROW_WEBSOCKET_ROUTE(app, "/ws")
      .onopen([&](crow::websocket::connection& conn) 
      {
        CROW_LOG_INFO << "new websocket connection from " << conn.get_remote_ip();
        std::lock_guard<std::mutex> _(mtx);
        users.insert(&conn);
      })
      .onclose([&](crow::websocket::connection& conn, const std::string& reason) 
      {
        CROW_LOG_INFO << "websocket connection closed: " << reason;
        std::lock_guard<std::mutex> _(mtx);
        users.erase(&conn);
      })
      .onmessage([&](crow::websocket::connection&, const std::string& data, bool is_binary) 
      {
        std::lock_guard<std::mutex> _(mtx);

        for (auto u : users)
            if (is_binary)
                u->send_binary(data);
            else
                u->send_text(data);
    });

    CROW_CATCHALL_ROUTE(app)([&](const crow::request req){

        std::string file = '.'+req.url;

        if (allow_log) 
            std::cout << "file requested: '" << req.url << "'\n";

        if (std::filesystem::exists(file))
        {
            crow::response res;

            if (std::filesystem::is_directory(file) && std::filesystem::exists(file+"/index.html"))
                file+="/index.html";
            else if (std::filesystem::is_directory(file))
            {
                if (allow_log) 
                    std::cout << "404 '" << file << "' not found\n";

                return crow::response(404);
            }

            if (std::filesystem::path(file).extension() == ".html")
            {
                res.set_header("Content-type", "text/html");

                res.body = read_file(file)+
                "\n<script>let s=new WebSocket(`ws://${window.location.host}/ws`);s.onopen=()=>{console.log('open')};s.onerror=(e)=>{console.log('error',e)};s.onclose=(e)=>{console.log('close',e)};s.onmessage=(e)=>{console.log(e.data);if(e.data==='r')location.reload();}</script>\n";
            }
            else res.set_static_file_info(file); 

            res.code = 200;
            return res;
        }

        if (allow_log) 
            std::cout << "404 '" << file << "' not found\n";

        return crow::response(404);
    });

    app.loglevel(crow::LogLevel::CRITICAL);
    auto _a = app.bindaddr(addr).port(port).multithreaded().run_async();

    std::string addr_str = addr;

    if (addr == "0.0.0.0")
        addr_str = "localhost";

    std::cout << "stride server started at http://" << addr_str << ":" << port << '\n';

    std::unordered_map<std::string, std::filesystem::file_time_type> file_write_times;
    std::string to_erase = "";

    while (_a.valid())
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(watch))
        {
            if (entry.is_regular_file() && !file_write_times.count(entry.path().string()))
            {
                const auto& path = entry.path();

                if (!ignore.empty())
                {
                    const std::string& str = origin_dir+path.string();
                    bool found = false;

                    for (const auto& item : ignore)
                    {
                        if (str.find(item) != std::string::npos)
                        {
                            found = true;
                            break;
                        }
                    }

                    if (found)
                        continue;
                }

                if (!entry.exists())
                    continue;

                file_write_times[path.string()] = std::filesystem::last_write_time(path);
                
                if (allow_log) 
                    std::cout << "now watching: " << path << '\n';
            }
        }

        for (auto pair : file_write_times)
        {
            if (std::filesystem::exists(pair.first) && pair.second != std::filesystem::last_write_time(pair.first))
            {
                if (!build.empty())
                {
                    if (allow_log)
                        std::cout << "file updated: running build command ("+build+")\n";

                    std::filesystem::current_path(origin_dir);
                    system(build.c_str());
                    std::filesystem::current_path(serve);

                    std::lock_guard<std::mutex> _(mtx);

                    for (auto u : users)
                        u->send_text("r");

                    file_write_times.clear();
                    break;
                }

                file_write_times[pair.first] = std::filesystem::last_write_time(pair.first);
                std::lock_guard<std::mutex> _(mtx);

                for (auto u : users)
                    u->send_text("r");
            }
            else if (!std::filesystem::exists(pair.first))
                to_erase = pair.first;
        }

        auto status = _a.wait_for(1ms);
        
        if (!to_erase.empty()) 
        {
            file_write_times.erase(to_erase);
            to_erase.clear();
        }

        if (status == std::future_status::ready) break;
    }

    std::cout << "\nshutting down...\n";
}