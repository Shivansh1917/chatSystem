#include <iostream>
#include <thread>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <sstream>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>
#include <errno.h>
#include <fstream>
#include "nlohmann/json.hpp"

#define PORT 6000
#define BUFFER_SIZE 1024

struct User {
    std::string username;
    std::string password;
    std::string ip;
    int port;
};

using namespace std;
using json = nlohmann::json;

std::unordered_map<std::string, User> users;
std::mutex users_mutex;

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

//============ save users info to json file ================================ 
void save_users_to_file() {
    lock_guard<mutex> lg(users_mutex);

    json j;

    for (auto& [username, user] : users) {
        j[username] = {
            {"password", user.password},
            {"ip", user.ip},
            {"port", user.port}
        };
    }

    ofstream out("users.json");
    out << j.dump(4);
}

//============ load users info from json file ==============================
void load_users_from_file() {

    ifstream in("users.json");
    if (!in.is_open())
        return;

    json j;
    in >> j;

    for (auto& [username, data] : j.items()) {

        User u;
        u.username = username;
        u.password = data["password"];
        u.ip = data["ip"];
        u.port = data["port"];

        users[username] = u;
    }
}

//============ client handler ===============================================
void handle_client(int sockfd,sockaddr_in client_addr){
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
    std::string client_ip = ip_str;
    int client_port = ntohs(client_addr.sin_port);

    cout << "Connection from IP: " << client_ip << endl;
    
    string command;
    if(!recv_message(sockfd,command)){
        cout << "Client closed the connection." <<endl;
        close(sockfd);
        return;
    }

    if(command=="REGISTER"){
        string username,password;
        if(!recv_message(sockfd,username)){
            cout << "Client closed the connection." <<endl;
            close(sockfd);
            return;
        }
        if(!recv_message(sockfd,password)){
            cout << "Client closed the connection." <<endl;
            close(sockfd);
            return;
        }

        bool already_exists;

        {
            lock_guard<mutex> lg(users_mutex);
            if(!(already_exists = (users.find(username)!=users.end()))){
                users[username] = {username,password,client_ip,client_port};
            }
        }

        if(already_exists){
            if(!send_message(sockfd,"USERNAME_EXISTS")){
                cout << "Client closed the connection." <<endl;
            }
            close(sockfd);
            return;
        }

        if(!send_message(sockfd,"SUCCESS")){
            cout << "Client closed the connection." <<endl;
            close(sockfd);
            return;
        }
        save_users_to_file(); //save this new user to the file
        close(sockfd);
        return;
    }
    
    else if(command=="LOGIN"){
        string username,password;
        if(!recv_message(sockfd,username)){
            cout << "Client closed the connection." <<endl;
            close(sockfd);
            return;
        }
        if(!recv_message(sockfd,password)){
            cout << "Client closed the connection." <<endl;
            close(sockfd);
            return;
        }

        bool valid_username,valid_password=false;

        {
            lock_guard<mutex> lg(users_mutex);

            auto it = users.find(username);
            valid_username = it!=users.end();
            if(valid_username && it->second.password == password){
                valid_password = true;
            }
        }

        if(!valid_username){  //username doesnt exist
            if(!send_message(sockfd,"NO_USER")){
                cout << "Client closed the connection." <<endl;
            }
            close(sockfd);
            return;
        }
        else if(!valid_password){
            if(!send_message(sockfd,"INVALID")){ //password is invalid
                cout << "Client closed the connection." <<endl;
            }
            close(sockfd);
            return;
        }

        //valid password
        if(!send_message(sockfd,"VALID")){
            cout << "Client closed the connection." <<endl;
            close(sockfd);
            return;
        }
        close(sockfd);
        return;
    }

    else{
        if(!send_message(sockfd,"Unknown command.")){
            cerr << "Client closed connection." <<endl;
        }
        close(sockfd);
        return;
    }
}

//============ main =========================================================
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

    if(::bind(server_fd, (sockaddr*)&address, sizeof(address))<0){
        cerr << "Error occured during binding." <<endl;
        close(server_fd);
        return 1;
    }

    if(listen(server_fd, 10)<0){
        cerr <<"Error occured during listen." <<endl;
        close(server_fd);
        return 1;
    }

    std::cout << "Discovery server running on port " << PORT << std::endl;

    cout << "Loading users info..." <<endl;
    load_users_from_file();
    cout << "Loaded users info successfully." << endl;

    while (true) {
        sockaddr_in client_addr{};
        socklen_t addrlen = sizeof(client_addr);

        int client_socket = accept(server_fd, (sockaddr*)&client_addr, &addrlen);

        if(client_socket<0){
            cerr << "Error occured during accept." <<endl;
            continue;
        }

        std::thread t(handle_client, client_socket, client_addr);
        t.detach(); 
    }

    close(server_fd);
    return 0;
}