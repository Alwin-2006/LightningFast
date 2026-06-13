#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>

static int on = 1, off = 0;

void handle_client(int c_fd){
        char rbuf[64] = {};
        int n = read(c_fd,&rbuf,sizeof(rbuf));
        if(n<0){
                perror("Couldn't read data from the client\n");
        }
        printf("Client says: %s\n",rbuf);

        char wbuf[64] = "Hello, from the server";
        n = write(c_fd,&wbuf,sizeof(wbuf));
        if(n<0){
                perror("Couldn't write data to the socket\n");
        }
}

int main(){
        int fd = socket(AF_INET,SOCK_STREAM,0);

        setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on));

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(1234);
        addr.sin_addr.s_addr = htonl(0);
        int n = bind(fd,(const struct sockaddr*)&addr,sizeof(addr));
        if(n){
                perror("Couldn't bind socket to address\n");
        }

        n = listen(fd,1024);
        if(n){
                perror("Couln't listen on the socket\n");
        }

        while(true){
                struct sockaddr_in c_addr = {};
                socklen_t s_len = sizeof(c_addr);
                int c_fd = accept(fd,(struct sockaddr*)&c_addr,&s_len);
                if(c_fd<0){
                        perror("Couldn't accept a client\n");
                }

                handle_client(c_fd);
        }
}