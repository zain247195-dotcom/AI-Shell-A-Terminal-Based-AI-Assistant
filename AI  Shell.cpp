#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <vector>
#include <sstream>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
#include <curl/curl.h>
#include "nlohmann/json.hpp"

using namespace std;
using json = nlohmann::json;

static size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    string* s = static_cast<string*>(userp);
    s->append(static_cast<char*>(contents), total);
    return total;
}

void print_typing(const string& text, useconds_t micros_per_char = 8000) {
    cout << "\nAI: ";
    for (char c : text) {
        cout << c << flush;
        usleep(micros_per_char);
    }
    cout << "\n\n";
}

string trim(const string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    if (start == string::npos) return "";
    return s.substr(start, end - start + 1);
}

vector<string> split_ws(const string& s) {
    istringstream iss(s);
    vector<string> parts;
    string tok;
    while (iss >> tok) parts.push_back(tok);
    return parts;
}

bool contains_dangerous_chars(const string& s) {
    return s.find(';') != string::npos ||
           s.find('|') != string::npos ||
           s.find('&') != string::npos ||
           s.find('`') != string::npos ||
           s.find("$(") != string::npos;
}

bool is_safe_command(const string& input) {
    string s = trim(input);
    if (s.empty()) return false;
    if (contains_dangerous_chars(s)) return false;

    vector<string> parts = split_ws(s);
    if (parts.empty()) return false;

    vector<string> allowed = {"ls", "pwd", "whoami", "date", "echo", "cat", "mkdir", "touch", "clear"};
    string cmd = parts[0];

    if (find(allowed.begin(), allowed.end(), cmd) == allowed.end())
        return false;

    if (cmd == "cat" && parts.size() > 2) return false;
    if (cmd == "mkdir" && parts.size() > 2) return false;
    if (cmd == "touch" && parts.size() > 2) return false;

    return true;
}

string run_safe_command(const string& input) {
    vector<string> parts = split_ws(trim(input));
    if (parts.empty()) return "Error: empty command";

    int pipefd[2];
    if (pipe(pipefd) == -1) return "Error: pipe failed";

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return "Error: fork failed";
    }

    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        vector<char*> args;
        for (auto& p : parts) args.push_back(const_cast<char*>(p.c_str()));
        args.push_back(nullptr);

        execvp(args[0], args.data());
        _exit(1);
    }

    close(pipefd[1]);

    string output;
    char buffer[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, n);
    }

    close(pipefd[0]);
    waitpid(pid, nullptr, 0);

    if (output.empty()) output = "[command executed]";
    return output;
}

string build_payload(const string& user_prompt) {
    json j;
    j["model"] = "llama-3.3-70b-versatile";
    j["messages"] = json::array({
        { {"role", "system"}, {"content", "You are a helpful assistant."} },
        { {"role", "user"}, {"content", user_prompt} }
    });
    return j.dump();
}

pair<bool, string> extract_content_from_response(const string& raw) {
    try {
        auto j = json::parse(raw);
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            auto first = j["choices"][0];
            if (first.contains("message") && first["message"].contains("content")) {
                return {true, first["message"]["content"].get<string>()};
            }
            if (first.contains("text")) {
                return {true, first["text"].get<string>()};
            }
        }
        return {false, "Error: unexpected JSON structure"};
    } catch (const exception& e) {
        return {false, string("Error parsing JSON: ") + e.what()};
    }
}

string send_request(const string& api_key, const string& payload, long& out_http_code, string& out_error_msg) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        out_error_msg = "curl_easy_init() failed";
        return "";
    }

    string response;
    curl_slist* headers = nullptr;
    string auth = "Authorization: Bearer " + api_key;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.groq.com/openai/v1/chat/completions");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        out_error_msg = string("curl_easy_perform() failed: ") + curl_easy_strerror(res);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return "";
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out_http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

bool looks_like_shell_command(const string& input) {
    string s = trim(input);
    if (s.empty()) return false;

    if (s.find(' ') == string::npos) {
        vector<string> single_cmds = {"ls", "pwd", "whoami", "date", "clear"};
        return find(single_cmds.begin(), single_cmds.end(), s) != single_cmds.end();
    }

    return is_safe_command(s);
}

int main() {
    const char* apikey_env = getenv("GROQ_API_KEY");
    if (!apikey_env) {
        cerr << "Error: GROQ_API_KEY environment variable not set.\n";
        cerr << "Export it first: export GROQ_API_KEY=\"your_key\"\n";
        return 1;
    }

    string api_key = apikey_env;

    cout << "AI Terminal Ready\n";
    cout << "Type anything to talk to AI (Ctrl+C to exit)\n";
    cout << "Safe shell commands are allowed: ls, pwd, whoami, date, echo, cat, mkdir, touch, clear\n\n";

    while (true) {
        cout << "> ";
        string input;
        if (!getline(cin, input)) break;

        input = trim(input);
        if (input.empty()) continue;

        if (looks_like_shell_command(input)) {
            if (!is_safe_command(input)) {
                cout << "Blocked: unsafe command\n\n";
                continue;
            }

            string out = run_safe_command(input);
            cout << "\nShell Output:\n" << out << "\n\n";
            continue;
        }

        string payload = build_payload(input);
        long http_code = 0;
        string curl_err;
        string raw = send_request(api_key, payload, http_code, curl_err);

        if (raw.empty()) {
            cerr << "Network error: " << curl_err << "\n";
            continue;
        }

        if (http_code < 200 || http_code >= 300) {
            cerr << "HTTP error " << http_code << ". Response: " << raw << "\n";
            continue;
        }

        auto res = extract_content_from_response(raw);
        if (!res.first) {
            cerr << res.second << "\nFull response: " << raw << "\n";
            continue;
        }

        print_typing(res.second);
    }

    return 0;
}