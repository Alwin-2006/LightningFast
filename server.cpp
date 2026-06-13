#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <iostream>
#include <vector>
#include <map>
#include <string>

struct RedisCommand {
    std::vector<std::string> args;
    bool is_complete = false;
};

static int on = 1;
std::map<std::string, std::string> redis_data_store; 


RedisCommand parse_resp_buffer(std::string& buffer) {
    RedisCommand cmd;
    if (buffer.empty() || buffer[0] != '*') return cmd; 
    size_t first_crlf = buffer.find("\r\n");
    if (first_crlf == std::string::npos) return cmd; 

    int num_elements = std::stoi(buffer.substr(1, first_crlf - 1));
    size_t cursor = first_crlf + 2; 

    for (int i = 0; i < num_elements; ++i) {
        if (cursor >= buffer.size() || buffer[cursor] != '$') return cmd; 
        size_t len_crlf = buffer.find("\r\n", cursor);
        if (len_crlf == std::string::npos) return cmd; 

        int str_len = std::stoi(buffer.substr(cursor + 1, len_crlf - (cursor + 1)));
        cursor = len_crlf + 2; 

        if (cursor + str_len + 2 > buffer.size()) return cmd; 

        std::string arg = buffer.substr(cursor, str_len);
        cmd.args.push_back(arg);
        cursor += str_len + 2; 
    }

    buffer.erase(0, cursor);
    cmd.is_complete = true;
    return cmd;
}

void handle_client(int c_fd) {
    std::string client_buffer = "";
    char raw_read_buf[1024]; // Safe primitive buffer for raw bytes

    // Loop to keep servicing this client until they disconnect
    while (true) {
        ssize_t n = read(c_fd, raw_read_buf, sizeof(raw_read_buf) - 1);
        
        if (n < 0) {
            perror("Couldn't read data from the client");
            break;
        }
        if (n == 0) {
            // Client closed connection (e.g. they exited redis-cli)
            std::cout << "Client disconnected.\n";
            break;
        }

        // Safely append raw bytes to our persistent string buffer
        client_buffer.append(raw_read_buf, n);

        RedisCommand cmd = parse_resp_buffer(client_buffer);

        if (cmd.is_complete) {
            if (!cmd.args.empty() && cmd.args[0] == "PING") {
                std::string response = "+PONG\r\n";
                write(c_fd, response.c_str(), response.length());
            } 
            else if(!cmd.args.empty()&& cmd.args[0]== "ECHO"){
                if(cmd.args.size()>=2){
                        for(int i = 2; i < cmd.args.size(); ++i) {
                            cmd.args[1] += " " + cmd.args[i]; 
                        }

                    std::string response = "$" + std::to_string(cmd.args[1].length()) + "\r\n" + cmd.args[1]+ "\r\n"; 
                    write(c_fd, response.c_str(), response.length());
                } else {
                    std::string response = "-ERR wrong number of arguments for 'ECHO' command\r\n";
                    write(c_fd, response.c_str(), response.length());
                }
            }
            else if(!cmd.args.empty() && cmd.args[0] == "SET"){
                if(cmd.args.size() >= 3){
                    redis_data_store[cmd.args[1]] = cmd.args[2];
                    std::string response = "+OK\r\n"; 
                    write(c_fd, response.c_str(), response.length());
                } else {
                    std::string response = "-ERR wrong number of arguments for 'SET' command\r\n";
                    write(c_fd, response.c_str(), response.length());
                }
            }
            else if(!cmd.args.empty() && cmd.args[0] == "GET"){
                if(cmd.args.size() >= 2){
                    auto it = redis_data_store.find(cmd.args[1]);
                    if(it != redis_data_store.end()){
                        std::string value = it->second;
                        std::string response = "$" + std::to_string(value.length()) + "\r\n" + value + "\r\n"; // Valid RESP Bulk String
                        write(c_fd, response.c_str(), response.length());
                    } else {
                        std::string response = "$13\r\nKey not found\r\n"; // lowkey made a mistake here, i tried to concatenate two raw strings like just "hello world" is a char []
                        write(c_fd, response.c_str(), response.length());
                    }
                } else {
                    std::string response = "-ERR wrong number of arguments for 'GET' command\r\n";
                    write(c_fd, response.c_str(), response.length());
                }
            }
            else {
                // Return a valid RESP error so redis-cli doesn't crash
                std::string response = "-ERR unknown command\r\n";
                write(c_fd, response.c_str(), response.length());
            }
        }
    }
    close(c_fd); // Don't forget to close the socket when finished!
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("Socket creation failed\n");
        return 1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(6379);
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // Binds to localhost cleanly

    int n = bind(fd, (const struct sockaddr*)&addr, sizeof(addr));
    if (n) {
        perror("Couldn't bind socket to address\n");
        return 1;
    }

    n = listen(fd, 1024); 
    if (n) {
        perror("Couldn't listen on the socket\n");
        return 1;
    }

    std::cout << "Redis server listening on port 6379...\n";

    while (true) {
        struct sockaddr_in c_addr = {};
        socklen_t s_len = sizeof(c_addr);
        int c_fd = accept(fd, (struct sockaddr*)&c_addr, &s_len);
        if (c_fd < 0) {
            perror("Couldn't accept a client\n");
            continue;
        }

        handle_client(c_fd);
    }
    close(fd);
    return 0;
}