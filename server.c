/** @file server.c */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <queue.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "queue.h"
#include "libhttp.h"
#include "libdictionary.h"

const char *HTTP_404_CONTENT = "<html><head><title>404 Not Found</title></head><body><h1>404 Not Found</h1>The requested resource could not be found but may be available again in the future.<div style=\"color: #eeeeee; font-size: 8pt;\">Actually, it probably won't ever be available unless this is showing up because of a bug in your program. :(</div></html>";
const char *HTTP_501_CONTENT = "<html><head><title>501 Not Implemented</title></head><body><h1>501 Not Implemented</h1>The server either does not recognise the request method, or it lacks the ability to fulfill the request.</body></html>";

const char *HTTP_200_STRING = "OK";
const char *HTTP_404_STRING = "Not Found";
const char *HTTP_501_STRING = "Not Implemented";

queue_t *q;
queue_t *qf;
int sock;

void intHandler(int sig){

	int i;
    void *ret;
    for(i=0;i < (int) queue_size(q);i++){
    	shutdown((intptr_t)queue_at(qf, i), SHUT_RDWR);
    	pthread_join(*(pthread_t*)queue_at(q, i), &ret);
    }

    for(i=0;i< (int) queue_size(q);i++){
    	free((pthread_t*)queue_at(q, i));
    }

    queue_destroy(q);
    queue_destroy(qf);
    free(qf);
    free(q);
    close(sock);

	exit(0);

	printf("%d\n", sig);
}

/**
 * Processes the request line of the HTTP header.
 * 
 * @param request The request line of the HTTP header.  This should be
 *                the first line of an HTTP request header and must
 *                NOT include the HTTP line terminator ("\r\n").
 *
 * @return The filename of the requested document or NULL if the
 *         request is not supported by the server.  If a filename
 *         is returned, the string must be free'd by a call to free().
 */
char* process_http_header_request(const char *request)
{
	// Ensure our request type is correct...
	if (strncmp(request, "GET ", 4) != 0)
		return NULL;

	// Ensure the function was called properly...
	assert( strstr(request, "\r") == NULL );
	assert( strstr(request, "\n") == NULL );

	// Find the length, minus "GET "(4) and " HTTP/1.1"(9)...
	int len = strlen(request) - 4 - 9;

	// Copy the filename portion to our new string...
	char *filename = malloc(len + 4);
	strcpy(filename, "web");
	strncat(filename, request + 4, len);
	filename[len+3] = '\0';

	// Prevent a directory attack...
	//  (You don't want someone to go to http://server:1234/../server.c to view your source code.)
	if (strstr(filename, ".."))
	{
		free(filename);
		return NULL;
	}

	return filename;
}

char *getFileType(char* request){
	if(strstr(request, ".html") != NULL) {
		return "text/html\0";
	}else if(strstr(request, ".css") != NULL){
		return"text/css";
	}else if(strstr(request, ".jpg") != NULL){
		return"image/jpeg\0";
	}else if(strstr(request, ".png") != NULL){
		return "image/png\0";
	}else{
		return "text/plain\0";
	}

}

void send501(http_t *httpResp, int fd){
	char *sendString = malloc(500);
	char *connection;
	if((connection = (char *)http_get_header(httpResp, "Connection")) ==NULL ){
    	strcpy(connection, "close\0");
    }

	sprintf(sendString, "HTTP/1.1 501 %s\r\nServer: CS/241\r\nMIME-version: 1.0\r\nContent-type: text/html\r\nContent-Length: %d\r\nConnection: %s\r\n\r\n", HTTP_501_STRING, ((int) strlen(HTTP_501_CONTENT)), connection);
	send(fd, sendString, strlen(sendString), 0);
	send(fd, HTTP_501_CONTENT, strlen(HTTP_501_CONTENT), 0);
	free(sendString);
}

void send404(http_t *httpResp, int fd){
	char *sendString = malloc(500);
	char *connection;
	if((connection = (char *)http_get_header(httpResp, "Connection")) ==NULL ){
    	strcpy(connection, "close\0");
    }

	sprintf(sendString, "HTTP/1.1 404 %s\r\nServer: CS/241\r\nMIME-version: 1.0\r\nContent-type: text/html\r\nContent-Length: %d\r\nConnection: %s\r\n\r\n", HTTP_404_STRING, ((int) strlen(HTTP_404_CONTENT)), connection);
	send(fd, sendString, strlen(sendString), 0);
	send(fd, HTTP_404_CONTENT, strlen(HTTP_404_CONTENT), 0);
	free(sendString);
}

void send200(char *filename, http_t *httpResp, int fd){
	char *sendString = malloc(500);
	char *connection;
	char *content;
	if((connection = (char *)http_get_header(httpResp, "Connection")) ==NULL ){
    	strcpy(connection, "close\0");
    }

	int strSize;
	int readSize;
	FILE *handler = fopen(filename,"rb");
	if (handler){
		fseek(handler,0,SEEK_END);
		strSize = ftell (handler);
		rewind(handler);
		content = malloc(strSize +1 );

		sprintf(sendString, "HTTP/1.1 200 %s\r\nServer: CS/241\r\nMIME-version: 1.0\r\nContent-type: %s\r\nContent-Length: %d\r\nConnection: %s\r\n\r\n", HTTP_200_STRING, getFileType(filename), strSize, connection);
		send(fd, sendString, strlen(sendString), 0);

		readSize = fread(content, sizeof(char), strSize, handler);
		content[strSize] = '\0';
		
		if (strSize != readSize) {
       		free(content);
       		content = NULL;
       	}

       	send(fd, content, strSize, 0);
       	free(content);
    }

    free(sendString);
    fclose(handler);
}

void *worker(void *arg){
	int fd = (intptr_t)arg;
	int res = 0;

	while(1){
		http_t *httpResp = malloc(sizeof(http_t));

		res = http_read(httpResp, fd);
		if(res<0) {
			http_free(httpResp);
    		free(httpResp);
			break;
		}

		char *filename = process_http_header_request(httpResp->status);

		if(filename == NULL) {
			send501(httpResp, fd);
		}else{
			if(strcmp(filename, "web/")==0){
				filename = realloc(filename, 15);
				strcpy(filename, "web/index.html\0");
			}

			if(access(filename, F_OK) == 0){
				send200(filename, httpResp, fd);
			}else {
				send404(httpResp, fd);
			}
		}

		free(filename);

    	char *connection;
		if((connection = (char *)http_get_header(httpResp, "Connection")) ==NULL ){
    		close(fd);
    		return NULL;
   		}

    	http_free(httpResp);
    	free(httpResp);

	}

	return NULL;
}


int main(int argc, char **argv)
{

	q = malloc(sizeof(queue_t));
	qf = malloc(sizeof(queue_t));
   	queue_init(q);
   	queue_init(qf);

	signal(SIGINT, intHandler);

	if (argc != 2)
	{
		fprintf(stderr, "Usage: %s [port number]\n", argv[0]);
		return 1;
	}

	int port = atoi(argv[1]);
	if (port <= 0 || port >= 65536)
	{
		fprintf(stderr, "Illegal port number.\n");
		return 1;
	}

    sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(port);

    if( bind(sock, (struct sockaddr*) &server, sizeof(server)) <0 ){
    	perror("binding socket\n");
    	exit(1);
    }

    if(listen( sock, 20) < 0){
    	perror("listening error\n");
    	exit(1);
    }

    struct sockaddr_in client;
    socklen_t clientlen = sizeof(client);
    
    int fd;
    while(1){
    	fd = accept( sock, (struct sockaddr *)&client, &clientlen);
    	pthread_t *thread = malloc(sizeof(pthread_t));
    	queue_enqueue(q, thread);
    	queue_enqueue(qf, (void *) (intptr_t)fd);

    	pthread_create(thread, NULL, worker, (void *) (intptr_t)fd);
    }
    
	return 0;
}