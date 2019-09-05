#include <iostream>
#include <memory>
#include <sstream>
#include <fstream>
#include <regex>
#include <future>
#include <vector>
#include <base64.h>
#include <json.hpp>
#include <cxxopts.hpp>
#include <curl/curl.h>

using namespace std;


static const regex urlMatcher("^(.+?):(\\d+?):(.+?):(.+?):(.+?):(.+?)/\\?(.+?)$");
static const regex timeMatcher("(\\d+\\.?\\d*)\\s?ms");

struct server_t {
    string hostName;
    uint16_t port;
    string protocol;
    string encryption;
    string obfs;
    string password;
    string obfsParam;
    string protoParam;
    string remarks;
    string group;
    float ping;

    server_t (string & url) {
        string serverDecode, paramString;
        smatch matches;
        serverDecode = base64_decode(url.substr(6));
        regex_search(serverDecode, matches, urlMatcher);
        hostName = matches[1].str();
        port = stoi(matches[2].str());
        protocol = matches[3].str();
        encryption = matches[4].str();
        obfs = matches[5].str();
        password = base64_decode(matches[6].str());

        istringstream paramStream(matches[7].str());
        string kv, key, value;
        uint8_t pos;
        while (getline(paramStream, kv, '&')) {
            pos = kv.find("=");
            key = kv.substr(0, pos);
            value = kv.substr(pos + 1);
            replace(value.begin(), value.end(), '+', '-');
            replace(value.begin(), value.end(), '/', '_');
            value = base64_decode(value);
            if (key == "obfsparam") {
                obfsParam = value;
            } else if (key == "protoparam") {
                protoParam = value;
            } else if (key == "remarks") {
                remarks = value;
            } else if (key == "group") {
                group = value;
            }
        }
        ping = 0.0;
    };
    shared_ptr<nlohmann::json> toJSON() {
        auto ret = make_shared<nlohmann::json>();
        (*ret)["server"] = hostName;
        (*ret)["server_port"] = port;
        (*ret)["protocol"] = protocol;
        (*ret)["method"] = encryption;
        (*ret)["obfs"] = obfs;
        (*ret)["password"] = password;
        (*ret)["remarks"] = remarks;
        (*ret)["group"] = group;
        (*ret)["local_port"] = 1080;
        (*ret)["timeout"] = 60;
        (*ret)["obfs_param"] = obfsParam;
        (*ret)["protocol_param"] = protoParam;
        return ret;
    }
};

typedef shared_ptr<server_t> server_ptr;

bool cmp (server_ptr a, server_ptr b) {
    return a->ping < b->ping;
};

bool doPing(server_ptr server) {
    string ret;
    string command = "ping -W1 -c1 " + server->hostName;
    auto fin = popen(command.c_str(), "r");
    char buff[512];
    smatch matchResult;

    while (fgets(buff, sizeof(buff), fin) != NULL) {
        ret.append(buff);
    }
    pclose(fin);
    if (regex_search(ret, matchResult, timeMatcher)) {
        server->ping = stof(matchResult[1].str());
        if (server->ping <= 0.0) {
            server->ping = 999.0;
            return false;
        }
        return true;
    } else {
        server->ping = 999.0;
        return false;
    }
}

size_t writeFunction(void *ptr, size_t size, size_t nmemb, std::string* data) {
    data->append((char*) ptr, size * nmemb);
    return size * nmemb;
}

int main(int argc, char * argv[]) {
    cxxopts::Options options("SSR subscriber", "To generate shadowsocksr configuration file via subscription URL.");
    options.add_options()
        ("u,url", "Subscription URL", cxxopts::value<string>())
        ("c,config", "Configuration file path", cxxopts::value<string>()->default_value("/etc/shadowsocksr/config.json"));
    auto args = options.parse(argc, argv);

    CURL * curl = curl_easy_init();
    string response;
    string decoded;

    curl_easy_setopt(curl, CURLOPT_URL, args["url"].as<string>().c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeFunction);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10);
    cout << "Fetching server list" << endl;
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    decoded = base64_decode(response);

    istringstream decodedServers(decoded);
    string serverString;
    vector<server_ptr> serverList;
    vector<shared_ptr<future<bool>>> futureList;
    
    while (getline(decodedServers, serverString)) {
        server_ptr tmp = make_shared<server_t>(serverString);
        serverList.push_back(tmp);
        futureList.push_back(make_shared<future<bool>>(async(launch::async, doPing, tmp)));
    }
    if (serverList.size() <= 0) {
        cout << "Empty server list" << endl;
        return 1;
    }
    cout << "Pinging" << endl;
    for (auto fut : futureList) {
        fut->wait();
    }
    sort(serverList.begin(), serverList.end(), cmp);
    for (int i = 0; i < serverList.size(); ++i) {
        cout << i + 1 << ":\t" << serverList[i]->remarks << " " << serverList[i]->ping << "ms" << endl;
    }
    string selectedString;
    uint8_t selected = 0;
    while (selected <= 0) {
        try {
            cin >> selectedString;
            selected = stoi(selectedString);
            if (selected > serverList.size() || selected <= 0) {
                selected = 0;
                cout << "Bad selection" << endl;
            }
        } catch(const invalid_argument e) {
            cout << "Bad selection!" << endl;
        }
    }
    cout << serverList[selected - 1]->remarks << " selected" << endl;
    string jsonString = serverList[selected - 1]->toJSON()->dump();
    ofstream fout(args["config"].as<string>());
    fout.write(jsonString.c_str(), jsonString.size());
    fout.close();
    auto restartProc = popen("sudo systemctl restart shadowsocksr@config", "r");
    cout << "Restarting shadowsocks" << endl;
    char buff[512];
    while (fgets(buff, sizeof(buff), restartProc) != NULL) {
    }
    pclose(restartProc);
    cout << "Done" << endl;
    return 0;
}
