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

#define BUFFER_SIZE 4096
#define RESET   "\033[0m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define CYAN    "\033[36m"

using namespace std;

atomic<bool> running(true);
mutex cout_mutex;

const string server_msg_color = RED;
const string everyone_colour = YELLOW;
const string private_msg_colour = CYAN;
const string sender_colour = GREEN;

unordered_set<string> valid_commands = {
    "$$EXIT",
    "$$SHOW_ONLINE_USERS",
    "$$PRIVATE",
    "$$EVERYONE",
    "$$HISTORY",
    "$$STATUS"
};

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

//============== text formatters ==============================
void trim_newline(string& str) {
    while (!str.empty() && (str.back() == '\n' || str.back() == '\r')) {
        str.pop_back();
    }
}

void trim_whitespaces(string& str){
    while(!str.empty() && (str.back()==' ' || str.back()=='\t')){
        str.pop_back();
    }
}

//============== get username =============================
void get_username(std::string& username)
{
    while (true)
    {
        std::cout << "Enter username: ";
        std::getline(std::cin, username);
        trim_newline(username);

        if (username.size()<3 || username.size()>16)
        {
            std::cout << "Username must be between 3 and 16 characters.\n";
            continue;
        }

        if (!std::isalnum(static_cast<unsigned char>(username[0])))
        {
            std::cout << "Username must start with an alphanumeric character.\n";
            continue;
        }

        if (username.back() == ' ')
        {
            std::cout << "Username cannot end with a space.\n";
            continue;
        }

        bool valid = true;
        int consecutive_spaces = 0;

        for (unsigned char c : username)
        {
            if (std::iscntrl(c))
            {
                valid = false;
                break;
            }

            if (c == ' ')
            {
                consecutive_spaces++;
                if (consecutive_spaces > 1)
                {
                    valid = false;
                    break;
                }
            }
            else
            {
                consecutive_spaces = 0;
            }
        }
        if (!valid)
        {
            std::cout << "Username cannot contain control characters or more than one consecutive space.\n";
            continue;
        }
        break; //valid username
    }
}

//============== get password =============================
void get_password(string& password){
    while(true){
        cout<<"Enter password: ";
        getline(cin, password);
        trim_newline(password);
        if(password.size()<8 or password.size()>16){
            cout << "Password should have at least 8 characters and maximum of 16 characters" <<endl;
            continue;
        }
        if(password.find(' ')!=string::npos){
            cout << "Password should not contain whitespaces" <<endl;
            continue;
        }
        break;
    }
}

//============== user registration ========================
bool user_registeration(){
    int sockfd;
    string server_ip;
    int server_port;
    struct sockaddr_in server_addr;

    //get discovery server ip and port from user
    while (true)
    {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            cerr << "Error creating socket" << endl;
            return false;
        }
        cout << "Enter discovery server IP: ";
        cin >> server_ip;
        string port;
        cout << "Enter discovery server port: ";
        cin >> port;
        cin.ignore(numeric_limits<streamsize>::max(), '\n'); // Clear the newline character from the input buffer

        //checking if the entered port is valid
        try{
            server_port = stoi(port);
        }
        catch(const std::exception& e){
            cerr << "Invalid port number. Please enter a valid port number." << endl;
            close(sockfd);
            continue;
        }

        if(server_port<1 || server_port>65535){
            cerr << "Port number must be between 1 and 65535. Please enter a valid port number." << endl;
            close(sockfd);
            continue;
        }
    
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        if(inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr)<=0) {
            cerr << "Invalid IP address" << endl;
            close(sockfd);
            continue;
        }
        if(connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            cerr << "Connection failed" << endl;
            close(sockfd);
            return false;
        }
        break;
    }

    string username,password;
    get_username(username);
    get_password(password);

    trim_newline(username);
    trim_whitespaces(username);

    if(!send_message(sockfd,"REGISTER")){
        cerr << "Failed to send registration info. Retrying..." << endl;
        close(sockfd);
        return false;
    }
    if(!send_message(sockfd,username)){
        cerr << "Failed to send registration info. Retrying..." << endl;
        close(sockfd);
        return false;
    }
    if(!send_message(sockfd,password)){
        cerr << "Failed to send registration info. Retrying..." << endl;
        close(sockfd);
        return false;
    }

    string response;
    if(!recv_message(sockfd, response)){
        cerr << "Failed to receive response from server." << endl;
        close(sockfd);
        return false;
    }
    else if(response=="SUCCESS"){
        cout << "Registration successful!" << endl;
        close(sockfd);
        return true;
    }
    else if(response=="SERVER_ERROR"){
        cerr << "Server error occurred during registration. Please try again later." << endl;
        close(sockfd);
        return false;
    }
    else if(response=="USERNAME_EXISTS"){
        cerr << "This username already exists. Please try another username." << endl;
        close(sockfd);
        return false;
    }
    cerr << "Registration failed." <<endl;
    close(sockfd);
    return false;
}

//============== user login ===============================
bool user_login(int sockfd,string& username){
    while(true){
        string password;
        get_username(username);
        get_password(password);

        if(!send_message(sockfd,"LOGIN")){
            cerr << "Failed to send login info. Retrying..." << endl;
            close(sockfd);
            return false;
        }
        if(!send_message(sockfd,username)){
            cerr << "Failed to send login info. Retrying..." << endl;
            close(sockfd);
            return false;
        }
        if(!send_message(sockfd,password)){
            cerr << "Failed to send login info. Retrying..." << endl;
            close(sockfd);
            return false;
        }

        string response;
        if(!recv_message(sockfd, response)){
            cout<<"Server closed the connection."<<endl;
            return false;
        }
        if(response=="SUCCESS"){
            cout << "Login successful!" << endl;
            cout << "Welcome " << username << "." <<endl;
            return true;
        }
        else if(response=="SERVER_ERROR"){
            cerr << "Server error occurred during login. Please try again later." << endl;
            return false;
        }
        else if(response=="INVALID_USERNAME"){
            cerr << "Invalid username. Please make sure to type the username correctly." << endl;
            continue;
        }
        else if(response=="INVALID_PASSWORD"){
            cerr << "Invalid password. Please try again." <<endl;
            continue;
        }
        else if(response=="USER_ALREADY_ONLINE"){
            cerr << "This user is already logged in." << endl;
            continue;
        }
        cerr << "Unexpected server response: " << response << endl;
        return false;
    }
    return false;
}

//============== threadsafe print function ================
void threadsafe_printer(const string& message,const string color_code="\033[0m",bool flush=false){
    lock_guard<mutex> lock(cout_mutex);
    cout << color_code << message << "\033[0m";
    if(flush) cout.flush();
}

//============== receiver thread ==========================
void receive_loop(int sockfd){
    string msg;
    while(running){
        if(!recv_message(sockfd, msg)){
            threadsafe_printer("Connection closed.\n",server_msg_color);
            running=false;
            break;
        }
        string s=msg;
        threadsafe_printer(s,"",true);
    }
}

//=============== main ==================================
int main() {

    int sockfd;
    string server_ip;
    int server_port;
    struct sockaddr_in server_addr;

    //get chat server ip and port from user
    while (true)
    {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            cerr << "Error creating socket" << endl;
            return 1;
        }
        cout << "Enter chat server IP: ";
        cin >> server_ip;
        string port;
        cout << "Enter chat server port: ";
        cin >> port;
        cin.ignore(numeric_limits<streamsize>::max(), '\n'); // Clear the newline character from the input buffer

        //checking if the entered port is valid
        try{
            server_port = stoi(port);
        }
        catch(const std::exception& e){
            cerr << "Invalid port number. Please enter a valid port number." << endl;
            close(sockfd);
            continue;
        }

        if(server_port<1 || server_port>65535){
            cerr << "Port number must be between 1 and 65535. Please enter a valid port number." << endl;
            close(sockfd);
            continue;
        }
    
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        if(inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr)<=0) {
            cerr << "Invalid IP address" << endl;
            close(sockfd);
            continue;
        }
        if(connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            cerr << "Connection failed" << endl;
            close(sockfd);
            return 1;
        }
        break;
    }
    cout << "Connected to server" << endl;

    while(true){
        cout << "Already registered? [y/n]: ";
        char choice;
        cin >> choice;
        cin.ignore(numeric_limits<streamsize>::max(), '\n'); // Clear the newline character from the input buffer
        if(tolower(choice)=='y'){
            break;
        }
        else if(tolower(choice)=='n'){
            while(!user_registeration());
            break;
        }
        else{
            cout << "Invalid choice. Please enter 'y' or 'n'." << endl;
        }
    }

    //user login
    string username;
    while(!user_login(sockfd,username));

    cout << endl;

    cout << server_msg_color
         << R"(List of commands:
$$SHOW_ONLINE_USERS    :   Shows a list of all online users
$$HISTORY              :   Shows your chat history
$$STATUS <status>      :   Sets your status to <status> (available/busy/away)
$$PRIVATE <username>   :   Switches to private chat with <username>
$$EVERYONE             :   Switches to everyone chat
$$EXIT                 :   To exit the chat

Note that the commands are case sensitive!
        )"
         << RESET << endl;

    thread receiver(receive_loop,sockfd);

    while(running){
        string input;

        if(!running) break;
        if(!getline(cin, input)){
            cout<<"getline failed\n";
            running = false;
            break;
        }

        if(input.substr(0,2)=="$$"){
            trim_whitespaces(input);
            bool valid=false;
            if(input.size()>=10 and input.substr(0,10)=="$$PRIVATE "){
                valid=true;
            }
            else if(input.size() >= 9 && input.substr(0,9) == "$$STATUS "){
                valid = true;
            }
            else if(!valid and valid_commands.find(input)==valid_commands.end()){
                threadsafe_printer("SERVER: Invalid command. Please enter a valid command\n",server_msg_color);
                continue;
            }
            if(input.substr(2,4)=="EXIT"){
                running=false;
                send_message(sockfd,"$$EXIT");
                break;
            }
        }
        if(!send_message(sockfd, input)){
            threadsafe_printer("SERVER: Failed to send message. Server might be down.\n",server_msg_color);
            running=false;
            break;
        }
    }

    receiver.join();
    threadsafe_printer("Exiting...\n",server_msg_color);
    close(sockfd);

    return 0;
}