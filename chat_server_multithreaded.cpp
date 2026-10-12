#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <mutex>
#include <limits>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <signal.h>
#include <errno.h>
#include <ctime>
#include <fstream>
#include <chrono>
#include "json.hpp"

#define PORT 5000
#define DISCOV_IP "127.0.0.1"
#define DISCOV_PORT 6000

using namespace std;
using json = nlohmann::json;

namespace colour {
    constexpr const char* RESET = "\033[0m";

    constexpr const char* SERVER   = "\033[38;5;75m";
    constexpr const char* EVERYONE = "\033[38;5;42m";
    constexpr const char* PRIVATE  = "\033[38;5;177m";
}

struct client_info{
    int socket;
    string username,private_target;
    bool private_mode=false;
    string status="avaliable";
};

struct ChatRecord{
    string timestamp;
    string type;     // "broadcast" or "private"
    string from;
    string to;       // empty for broadcast
    string message;
};

unordered_map<string, vector<ChatRecord>> chat_history;
mutex history_mutex;

unordered_map<string,client_info> online_users;
mutex users_mutex;

//============= tiemstamp ========================================
string current_timestamp() {
    time_t now = time(nullptr);

    struct tm result;                 // thread-local storage
    localtime_r(&now, &result);       // thread-safe conversion

    char buffer[20];                  // "YYYY-MM-DD HH:MM:SS" + '\0'
    strftime(buffer,sizeof(buffer),"%Y-%m-%d %H:%M:%S",&result);
    
    return string(buffer);
}

//============== text formatting ===================================
void colourise(std::string& msg, const char* colour_code) {
    msg.insert(0, colour_code);
    msg.append(colour::RESET);
}

void trim_newline(string& str) {
    while (!str.empty() && (str.back() == '\n' || str.back() == '\r')) {
        str.pop_back();
    }
}

void trim_whitespace(string& str){
    while(!str.empty() && (str.back()==' ' || str.back()=='\t')){
        str.pop_back();
    }
}

string to_lowercase(string s) {
    transform(s.begin(), s.end(), s.begin(),
              [](unsigned char c){ return std::tolower(c); });
    return s;
}

//============== helpers ===========================================
ssize_t send_all(int sockfd, const void* buffer, size_t length) {
    size_t total = 0;
    while(total < length){
        ssize_t n = send(sockfd,(const char*)buffer + total,length - total,0);
        if (n < 0) {
            if (errno == EINTR)
                continue;            // retry
            return -1;               // real error
        }
        if (n == 0) {
            return 0;                // connection closed
        }
        total += n;
    }
    return total;
}

bool send_message(int sockfd, const std::string& message) {
    uint32_t len = htonl(message.size());

    // Send length prefix
    if (send_all(sockfd, &len, sizeof(len)) <= 0)
        return false;

    // Send payload
    if (!message.empty()) {
        if (send_all(sockfd, message.data(), message.size()) <= 0)
            return false;
    }
    return true;
}

ssize_t recv_all(int sockfd, void* buffer, size_t length) {
    size_t total = 0;
    while (total < length) {
        ssize_t n = recv(sockfd,(char*)buffer + total,length - total,0);
        if (n < 0) {
            if (errno == EINTR)
                continue;            // retry
            return -1;               // real error
        }
        if (n == 0) {
            return 0;                // peer closed connection
        }
        total += n;
    }
    return total;
}

bool recv_message(int sockfd, std::string& message) {
    uint32_t len;

    if (recv_all(sockfd, &len, sizeof(len)) <= 0)
        return false;

    len = ntohl(len);

    if(len==0){
        message.clear();
        return true;
    }

    const uint32_t MAX_MSG_SIZE = 1 << 20; // 1MB

    if (len > MAX_MSG_SIZE)
        return false;

    std::vector<char> buffer(len);

    if (recv_all(sockfd, buffer.data(), len) <= 0)
        return false;

    message.assign(buffer.begin(), buffer.end());
    return true;
}

//============== server heleprs ================================
//sending server msg to a particular user
void send_to_user(const std::string& username, const std::string& msg) {
    string formatted_msg="SERVER: "+msg;
    colourise(formatted_msg,colour::SERVER);

    decltype(online_users.begin()) it;

    int sock=-2;
    {
        std::lock_guard<std::mutex> lock(users_mutex);
        it=online_users.find(username);
        if (it != online_users.end()) {
            sock=it->second.socket;
        }
    }
    
    if(sock>=0){
        send_message(sock,formatted_msg);
    }
}

//broadcast a msg to all the online clients
// trims newlines and whitespaces is not done at the end of msg
void broadcast_message(const string& sender,const string& msg) {
    string formatted = sender + ": " + msg + "\n";
    colourise(formatted,(sender=="SERVER") ? colour::SERVER : colour::EVERYONE);

    vector<int> sockets;

    //takign a snapshot of the sockets
    {
        lock_guard<std::mutex> lock(users_mutex);
        for (const auto& [uname, info] : online_users) {
            if (uname != sender) {
                sockets.push_back(info.socket);
            }
        }
    }

    {   // saving it to chat history
        lock_guard<mutex> lock(history_mutex);

        for (auto& [uname, _] : online_users) {
            ChatRecord rec;
            rec.timestamp = current_timestamp();
            rec.type = "broadcast";
            rec.from = sender;
            rec.to = "";
            rec.message = msg;

            chat_history[uname].push_back(rec);
        }
    }

    for(int sock:sockets){
        send_message(sock,formatted);
    }

}

//send private msg. formatting is done here itself
//changed return a little
bool send_private(const std::string& sender,const std::string& target,const std::string& msg) {
    std::string formatted ="(PRIVATE) " + sender + ": " + msg + "\n";
    colourise(formatted,colour::PRIVATE);

    decltype(online_users.begin()) it;
    int sock=-2;

    {
        std::lock_guard<std::mutex> lock(users_mutex);
        it=online_users.find(target);
        if(it==online_users.end()){
            return false;
        }
        sock=it->second.socket;
    }

    {
        lock_guard<mutex> lock(history_mutex);

        ChatRecord rec_sender;
        rec_sender.timestamp = current_timestamp();
        rec_sender.type = "private";
        rec_sender.from = sender;
        rec_sender.to = target;
        rec_sender.message = msg;

        chat_history[sender].push_back(rec_sender);
        chat_history[target].push_back(rec_sender);
    }

    return sock>=0 and send_message(sock, formatted); //idk if this is correct pls check
}

//sends the list of all online users to the given user
void send_online_users(int sockfd) {
    std::string list = "SERVER: Online Users:\n";
    
    {
        std::lock_guard<std::mutex> lock(users_mutex);
        for (const auto& [uname, info] : online_users) {
            list += " - " + uname + " (" + info.status + ")\n";
        }
    }

    colourise(list,colour::SERVER);
    send_message(sockfd, list);
}

//saves history to json file
void save_history_to_file() {
    lock_guard<mutex> lock(history_mutex);

    json j;

    for (auto& [username, records] : chat_history) {
        for (auto& rec : records) {
            j[username].push_back({
                {"timestamp", rec.timestamp},
                {"type", rec.type},
                {"from", rec.from},
                {"to", rec.to},
                {"message", rec.message}
            });
        }
    }

    ofstream out("chat_history.json");
    out << j.dump(4);
}

//loads history from file
void load_history_from_file() {
    ifstream in("chat_history.json");
    if (!in.is_open())
        return;

    json j;
    in >> j;

    for (auto& [username, records] : j.items()) {
        for (auto& item : records) {

            ChatRecord rec;
            rec.timestamp = item["timestamp"];
            rec.type = item["type"];
            rec.from = item["from"];
            rec.to = item["to"];
            rec.message = item["message"];

            chat_history[username].push_back(rec);
        }
    }
}

//===== user authentication =============================
bool authenticate_user(int client_sock,string& username_out){
    string username,password,command;

    //expect command (LOGIN)
    if (!recv_message(client_sock, command))
        return false;

    //expect username
    if (!recv_message(client_sock, username))
        return false;

    // Expect password
    if (!recv_message(client_sock, password))
        return false;

    trim_newline(username);
    trim_newline(password);

    int ds_sock = socket(AF_INET,SOCK_STREAM,0);

    if(ds_sock<0){
        perror("discovery server connect");
        return false;
    }

    struct sockaddr_in ds_addr;
    memset(&ds_addr, 0, sizeof(ds_addr));

    ds_addr.sin_family = AF_INET;
    ds_addr.sin_port   = htons(DISCOV_PORT);

    if (inet_pton(AF_INET, DISCOV_IP, &ds_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(ds_sock);
        return false;
    }

    if (connect(ds_sock,(struct sockaddr*)&ds_addr,sizeof(ds_addr)) < 0) {
        perror("connect");
        close(ds_sock);
        return false;
    }

    string response;

    send_message(ds_sock, "LOGIN");
    send_message(ds_sock, username);
    send_message(ds_sock, password);

    if (!recv_message(ds_sock, response)) {
        close(ds_sock);
        return false;
    }

    close(ds_sock);
    

    if(response=="VALID"){
        // Check username collision in chat server
        {
            std::lock_guard<std::mutex> lock(users_mutex);

            if (online_users.count(username)) {
                send_message(client_sock,"USER_ALREADY_ONLINE");
                return false;
            }
        }

        send_message(client_sock,"SUCCESS");

        username_out = username;
        return true;
    }
    else if(response=="INVALID"){
        send_message(client_sock,"INVALID_PASSWORD");
        return false;
    }
    else if(response=="NO_USER"){
        send_message(client_sock,"INVALID_USERNAME");
        return false;
    }
    return false;
}

//===== client handler =======================================
void handle_client(int client_sock){
    string username;
    
    //user login - give only a fixed number of trails
    //also doesn't block if the user leaves abruptly
    int trails=30;
    bool passed;
    while(trails>0){
        passed=authenticate_user(client_sock,username);
        if(passed) break;
        trails--;
    }

    if(!passed){    
        close(client_sock);
        return;
    }

    //add this user to online users list
    {
        std::lock_guard<std::mutex> lock(users_mutex);
        online_users[username] = {client_sock, username, "", false};
    }

    string msg=username+" has joined the chat.";
    broadcast_message("SERVER",msg);

    while(true){
        if(!recv_message(client_sock,msg)) break;

        trim_newline(msg);
        trim_whitespace(msg);

        if(msg.empty()) continue;

        //handling commands
        if(msg.substr(0,2)=="$$"){

            if(msg=="$$SHOW_ONLINE_USERS"){
                send_online_users(client_sock);
            }

            else if(msg.size()>10 and msg.substr(0,10)=="$$PRIVATE "){
                string target=msg.substr(10);
                trim_newline(target);
                trim_whitespace(target);

                bool found=false;
                {
                    lock_guard<mutex> lock(users_mutex);
    
                    auto it=online_users.find(target);
                    found=it!=online_users.end();
                    if(found){
                        online_users[username].private_mode = true;
                        online_users[username].private_target = target;
                    }
                }

                if(!found){
                    string s="This username is either invalid or the user is not online currently.";
                    send_to_user(username,s);
                }
            }

            else if(msg=="$$EVERYONE"){
                std::lock_guard<std::mutex> lock(users_mutex);

                online_users[username].private_mode = false;
                online_users[username].private_target.clear();
            }

            else if(msg == "$$HISTORY") {
                string output = "SERVER: Your Chat History:\n";
                {
                    lock_guard<mutex> lock(history_mutex);
                    for(const auto& rec : chat_history[username]) {
                        output += "[" + rec.timestamp + "] ";
                        if(rec.type == "broadcast") {
                            output += rec.from + ": " + rec.message + "\n";
                        }
                        else {
                            output += "(PRIVATE) " + rec.from + ": " + rec.message + "\n";
                        }
                    }
                }
                send_message(client_sock, output);
            }

            else if(msg.size() > 9 && msg.substr(0,9) == "$$STATUS ") {
                string new_status = msg.substr(9);
                trim_newline(new_status);
                trim_whitespace(new_status);
                new_status=to_lowercase(new_status);

                if(new_status=="available" || new_status=="busy" || new_status=="away") {
                    {
                        lock_guard<mutex> lock(users_mutex);
                        online_users[username].status = new_status;
                    }
                    send_to_user(username, "Status updated to " + new_status);
                }
                else {
                    send_to_user(username, "Invalid status.Usage: $$STATUS <available/busy/away>");
                }
            }

            else if(msg=="$$EXIT"){
                break;
            }
        }

        //handling normal messages
        else{
            bool private_mode;
            std::string target;

            {
                std::lock_guard<std::mutex> lock(users_mutex);
                private_mode = online_users[username].private_mode;
                target = online_users[username].private_target;
            }

            if(private_mode){
                if (!send_private(username, target, msg)) {
                    string s=target+" is offline.";
                    send_to_user(username,s);
                }
            }

            else{
                broadcast_message(username,msg);
            }
        }
    }

    close(client_sock);

     {
        std::lock_guard<std::mutex> lock(users_mutex);
        online_users.erase(username);
    }

    string s=username+" has disconnected.";
    broadcast_message("SERVER",s);
}

//===== persist the chat periodically to json file ===========
void persistence_thread() {
    while (true) {
        this_thread::sleep_for(chrono::seconds(5));
        save_history_to_file();
    }
}

//===== main =====================================================
int main(){

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if(server_fd<0){
        cerr << "Error occured during socket creation." <<endl;
        return 1;
    }

    // Allow quick restart of server
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET,SO_REUSEADDR,&opt, sizeof(opt)) < 0) {
        cerr << "Error occured during setsockopt." <<endl;
        close(server_fd);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if(bind(server_fd, (sockaddr*)&address, sizeof(address))<0){
        cerr << "Error occured during binding." <<endl;
        close(server_fd);
        return 1;
    }

    if(listen(server_fd, 10)<0){
        cerr <<"Error occured during listen." <<endl;
        close(server_fd);
        return 1;
    }

    cout << "Server has started..." << endl;

    load_history_from_file();

    thread t(persistence_thread);
    t.detach();

    while(true){
        sockaddr_in client_addr;
        socklen_t client_len=sizeof(client_addr);

        int client_sock=accept(server_fd,(sockaddr*)&client_addr,&client_len);

        if(client_sock<0){
            perror("accept");
            continue;
        }

        thread t(handle_client,client_sock);
        t.detach();
    }

    cout << "Closing server..." << endl;

    close(server_fd);
    return 0;
}