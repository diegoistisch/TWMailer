#define _DEFAULT_SOURCE
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

///////////////////////////////////////////////////////////////////////////////

#define BUF 1024

///////////////////////////////////////////////////////////////////////////////

int abortRequested = 0;
int create_socket = -1;
int new_socket = -1;
char *mailSpoolDir = NULL;

///////////////////////////////////////////////////////////////////////////////

void *clientCommunication(void *data);
void signalHandler(int sig);
int handleSend(int socket);
int getNextMessageNumber(const char *userDir);

///////////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv)
{
   socklen_t addrlen;
   struct sockaddr_in address, cliaddress;
   int reuseValue = 1;
   int port;

   ////////////////////////////////////////////////////////////////////////////
   // CHECK ARGUMENTS
   if (argc != 3)
   {
      fprintf(stderr, "Usage: %s <port> <mail-spool-directoryname>\n", argv[0]);
      return EXIT_FAILURE;
   }

   port = atoi(argv[1]);
   if (port <= 0 || port > 65535)
   {
      fprintf(stderr, "Error: Invalid port number\n");
      return EXIT_FAILURE;
   }

   mailSpoolDir = argv[2];

   ////////////////////////////////////////////////////////////////////////////
   // SIGNAL HANDLER
   // SIGINT (Interrup: ctrl+c)
   // https://man7.org/linux/man-pages/man2/signal.2.html
   if (signal(SIGINT, signalHandler) == SIG_ERR)
   {
      perror("signal can not be registered");
      return EXIT_FAILURE;
   }

   ////////////////////////////////////////////////////////////////////////////
   // CREATE A SOCKET
   // https://man7.org/linux/man-pages/man2/socket.2.html
   // https://man7.org/linux/man-pages/man7/ip.7.html
   // https://man7.org/linux/man-pages/man7/tcp.7.html
   // IPv4, TCP (connection oriented), IP (same as client)
   if ((create_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1)
   {
      perror("Socket error"); // errno set by socket()
      return EXIT_FAILURE;
   }

   ////////////////////////////////////////////////////////////////////////////
   // SET SOCKET OPTIONS
   // https://man7.org/linux/man-pages/man2/setsockopt.2.html
   // https://man7.org/linux/man-pages/man7/socket.7.html
   // socket, level, optname, optvalue, optlen
   if (setsockopt(create_socket,
                  SOL_SOCKET,
                  SO_REUSEADDR,
                  &reuseValue,
                  sizeof(reuseValue)) == -1)
   {
      perror("set socket options - reuseAddr");
      return EXIT_FAILURE;
   }

   if (setsockopt(create_socket,
                  SOL_SOCKET,
                  SO_REUSEPORT,
                  &reuseValue,
                  sizeof(reuseValue)) == -1)
   {
      perror("set socket options - reusePort");
      return EXIT_FAILURE;
   }

   ////////////////////////////////////////////////////////////////////////////
   // INIT ADDRESS
   // Attention: network byte order => big endian
   memset(&address, 0, sizeof(address));
   address.sin_family = AF_INET;
   address.sin_addr.s_addr = INADDR_ANY;
   address.sin_port = htons(port);

   ////////////////////////////////////////////////////////////////////////////
   // ASSIGN AN ADDRESS WITH PORT TO SOCKET
   if (bind(create_socket, (struct sockaddr *)&address, sizeof(address)) == -1)
   {
      perror("bind error");
      return EXIT_FAILURE;
   }

   ////////////////////////////////////////////////////////////////////////////
   // ALLOW CONNECTION ESTABLISHING
   // Socket, Backlog (= count of waiting connections allowed)
   if (listen(create_socket, 5) == -1)
   {
      perror("listen error");
      return EXIT_FAILURE;
   }

   while (!abortRequested)
   {
      /////////////////////////////////////////////////////////////////////////
      // ignore errors here... because only information message
      // https://linux.die.net/man/3/printf
      printf("Waiting for connections...\n");

      /////////////////////////////////////////////////////////////////////////
      // ACCEPTS CONNECTION SETUP
      // blocking, might have an accept-error on ctrl+c
      addrlen = sizeof(struct sockaddr_in);
      if ((new_socket = accept(create_socket,
                               (struct sockaddr *)&cliaddress,
                               &addrlen)) == -1)
      {
         if (abortRequested)
         {
            perror("accept error after aborted");
         }
         else
         {
            perror("accept error");
         }
         break;
      }

      /////////////////////////////////////////////////////////////////////////
      // START CLIENT
      // ignore printf error handling
      printf("Client connected from %s:%d...\n",
             inet_ntoa(cliaddress.sin_addr),
             ntohs(cliaddress.sin_port));
      clientCommunication(&new_socket); // returnValue can be ignored
      new_socket = -1;
   }

   // frees the descriptor
   if (create_socket != -1)
   {
      if (shutdown(create_socket, SHUT_RDWR) == -1)
      {
         perror("shutdown create_socket");
      }
      if (close(create_socket) == -1)
      {
         perror("close create_socket");
      }
      create_socket = -1;
   }

   return EXIT_SUCCESS;
}

void *clientCommunication(void *data)
{
   char buffer[BUF];
   int size;
   int *current_socket = (int *)data;

   ////////////////////////////////////////////////////////////////////////////
   // SEND welcome message
   strcpy(buffer, "Welcome to TWMailer!\r\n");
   if (send(*current_socket, buffer, strlen(buffer), 0) == -1)
   {
      perror("send failed");
      return NULL;
   }

   do
   {
      /////////////////////////////////////////////////////////////////////////
      // RECEIVE
      size = recv(*current_socket, buffer, BUF - 1, 0);
      if (size == -1)
      {
         if (abortRequested)
         {
            perror("recv error after aborted");
         }
         else
         {
            perror("recv error");
         }
         break;
      }

      if (size == 0)
      {
         printf("Client closed remote socket\n"); // ignore error
         break;
      }

      // remove newline
      if (buffer[size - 2] == '\r' && buffer[size - 1] == '\n')
      {
         size -= 2;
      }
      else if (buffer[size - 1] == '\n')
      {
         --size;
      }

      buffer[size] = '\0';
      printf("Command received: %s\n", buffer); // ignore error

      // COMMAND PARSING AB HIER
      if (strcmp(buffer, "SEND") == 0)
      {
         if (handleSend(*current_socket) == -1)
         {
            if (send(*current_socket, "ERR\n", 4, 0) == -1)
            {
               perror("send error response failed");
            }
         }
      }
      else if (strcmp(buffer, "LIST") == 0)
      {
         // TODO: LIST
         if (send(*current_socket, "OK\n", 3, 0) == -1)
         {
            perror("send answer failed");
            return NULL;
         }
      }
      else if (strcmp(buffer, "READ") == 0)
      {
         // TODO: READ
         if (send(*current_socket, "OK\n", 3, 0) == -1)
         {
            perror("send answer failed");
            return NULL;
         }
      }
      else if (strcmp(buffer, "DEL") == 0)
      {
         // TODO: DEL
         if (send(*current_socket, "OK\n", 3, 0) == -1)
         {
            perror("send answer failed");
            return NULL;
         }
      }
      else if (strcmp(buffer, "QUIT") == 0)
      {
         printf("Client requested QUIT\n");
         break;
      }
      else
      {
         if (send(*current_socket, "ERR\n", 4, 0) == -1)
         {
            perror("send answer failed");
            return NULL;
         }
      }
   } while (!abortRequested);

   // closes/frees the descriptor if not already
   if (*current_socket != -1)
   {
      if (shutdown(*current_socket, SHUT_RDWR) == -1)
      {
         perror("shutdown new_socket");
      }
      if (close(*current_socket) == -1)
      {
         perror("close new_socket");
      }
      *current_socket = -1;
   }

   return NULL;
}


//  funktion um die nächste nachrichtennummer für einen benutzer zu bekommen
int getNextMessageNumber(const char *userDir)
{
   DIR *dir;
   struct dirent *entry;
   int maxNum = 0;
   int currentNum;

   dir = opendir(userDir);
   if (dir == NULL)
   {
      return 1; 
   }

   while ((entry = readdir(dir)) != NULL)
   {
      // Check ob filename format "number.txt"
      if (sscanf(entry->d_name, "%d.txt", &currentNum) == 1)
      {
         if (currentNum > maxNum)
         {
            maxNum = currentNum;
         }
      }
   }

   closedir(dir);
   return maxNum + 1;
}


// funktion um den SEND command zu verarbeiten
// format:
// SEND
// username
// subject
// message (mehrere zeilen)
// .
int handleSend(int socket)
{
   char buffer[BUF];
   char username[9]; // Max 8 characters + null terminator
   char subject[81]; // Max 80 characters + null terminator
   char message[BUF * 10]; // um längere nachrichten zu erlauben
   char userDir[256];
   char filePath[300];
   int size;
   FILE *file;
   int messageNum;

   memset(message, 0, sizeof(message));

   // Receive username
   size = recv(socket, buffer, BUF - 1, 0);
   if (size <= 0)
   {
      perror("recv username failed");
      return -1;
   }
   
   // Remove newline
   if (buffer[size - 2] == '\r' && buffer[size - 1] == '\n')
   {
      size -= 2;
   }
   else if (buffer[size - 1] == '\n')
   {
      --size;
   }
   buffer[size] = '\0';

   // Validate username (max 8 characters)
   if (size > 8 || size == 0)
   {
      fprintf(stderr, "Invalid username length: %d\n", size);
      return -1;
   }
   strncpy(username, buffer, sizeof(username) - 1);
   username[sizeof(username) - 1] = '\0';
   printf("Username: %s\n", username);

   // Receive subject
   size = recv(socket, buffer, BUF - 1, 0);
   if (size <= 0)
   {
      perror("recv subject failed");
      return -1;
   }

   // Remove newline
   if (buffer[size - 2] == '\r' && buffer[size - 1] == '\n')
   {
      size -= 2;
   }
   else if (buffer[size - 1] == '\n')
   {
      --size;
   }
   buffer[size] = '\0';

   // Validate subject (max 80 characters)
   if (size > 80 || size == 0)
   {
      fprintf(stderr, "Invalid subject length: %d\n", size);
      return -1;
   }
   strncpy(subject, buffer, sizeof(subject) - 1);
   subject[sizeof(subject) - 1] = '\0';
   printf("Subject: %s\n", subject);

   // 
   int messageLen = 0;
   while (1)
   {
      size = recv(socket, buffer, BUF - 1, 0);
      if (size <= 0)
      {
         perror("recv message failed");
         return -1;
      }

      // Remove newline
      if (buffer[size - 2] == '\r' && buffer[size - 1] == '\n')
      {
         size -= 2;
      }
      else if (buffer[size - 1] == '\n')
      {
         --size;
      }
      buffer[size] = '\0';

      // Ccheckt für den End-of-message Marker
      if (strcmp(buffer, ".") == 0)
      {
         break;
      }

      // nachricht anhaengen
      if (messageLen + size + 1 < (int)sizeof(message))
      {
         if (messageLen > 0)
         {
            message[messageLen++] = '\n';
         }
         strncpy(message + messageLen, buffer, sizeof(message) - messageLen - 1);
         messageLen += size;
      }
      else
      {
         fprintf(stderr, "Message too long\n");
         return -1;
      }
   }
   printf("Message received (%d bytes)\n", messageLen);

   // Falls Benutzerverzeichnis noch nicht existiert, wird es erstellt
   snprintf(userDir, sizeof(userDir), "%s/%s", mailSpoolDir, username);
   
   // Create directory mit permissions 0700
   if (mkdir(userDir, 0700) == -1)
   {
      if (errno != EEXIST)
      {
         perror("mkdir failed");
         return -1;
      }
   }

   
   messageNum = getNextMessageNumber(userDir);
   
   // erstellt file path
   snprintf(filePath, sizeof(filePath), "%s/%d.txt", userDir, messageNum);

   // schreibt ins file
   file = fopen(filePath, "w");
   if (file == NULL)
   {
      perror("fopen failed");
      return -1;
   }

   // formatiert nachricht im file
   fprintf(file, "%s\n%s\n%s\n", username, subject, message);
   fclose(file);

   printf("Message saved to: %s\n", filePath);

   if (send(socket, "OK\n", 3, 0) == -1)
   {
      perror("send OK failed");
      return -1;
   }

   return 0;
}

void signalHandler(int sig)
{
   if (sig == SIGINT)
   {
      printf("abort Requested... "); // ignore error
      abortRequested = 1;
      /////////////////////////////////////////////////////////////////////////
      // With shutdown() one can initiate normal TCP close sequence ignoring
      // the reference count.
      // https://beej.us/guide/bgnet/html/#close-and-shutdownget-outta-my-face
      // https://linux.die.net/man/3/shutdown
      if (new_socket != -1)
      {
         if (shutdown(new_socket, SHUT_RDWR) == -1)
         {
            perror("shutdown new_socket");
         }
         if (close(new_socket) == -1)
         {
            perror("close new_socket");
         }
         new_socket = -1;
      }

      if (create_socket != -1)
      {
         if (shutdown(create_socket, SHUT_RDWR) == -1)
         {
            perror("shutdown create_socket");
         }
         if (close(create_socket) == -1)
         {
            perror("close create_socket");
         }
         create_socket = -1;
      }
   }
   else
   {
      exit(sig);
   }
}
