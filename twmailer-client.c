#define _DEFAULT_SOURCE

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////

#define BUF 1024

///////////////////////////////////////////////////////////////////////////////

int handleSendCommand(int socket);

///////////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv)
{
   int create_socket;
   char buffer[BUF];
   struct sockaddr_in address;
   int size;
   int isQuit;
   int port;

   ////////////////////////////////////////////////////////////////////////////
   // CHECK ARGUMENTS
   if (argc != 3)
   {
      fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]);
      return EXIT_FAILURE;
   }

   port = atoi(argv[2]);
   if (port <= 0 || port > 65535)
   {
      fprintf(stderr, "Error: Invalid port number\n");
      return EXIT_FAILURE;
   }

   ////////////////////////////////////////////////////////////////////////////
   // CREATE A SOCKET
   // https://man7.org/linux/man-pages/man2/socket.2.html
   // https://man7.org/linux/man-pages/man7/ip.7.html
   // https://man7.org/linux/man-pages/man7/tcp.7.html
   // IPv4, TCP (connection oriented), IP (same as server)
   if ((create_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1)
   {
      perror("Socket error");
      return EXIT_FAILURE;
   }

   ////////////////////////////////////////////////////////////////////////////
   // INIT ADDRESS
   // Attention: network byte order => big endian
   memset(&address, 0, sizeof(address)); // init storage with 0
   address.sin_family = AF_INET;         // IPv4
   // https://man7.org/linux/man-pages/man3/htons.3.html
   address.sin_port = htons(port);
   // https://man7.org/linux/man-pages/man3/inet_aton.3.html
   if (inet_aton(argv[1], &address.sin_addr) == 0)
   {
      fprintf(stderr, "Error: Invalid IP address\n");
      return EXIT_FAILURE;
   }

   ////////////////////////////////////////////////////////////////////////////
   // CREATE A CONNECTION
   // https://man7.org/linux/man-pages/man2/connect.2.html
   if (connect(create_socket,
               (struct sockaddr *)&address,
               sizeof(address)) == -1)
   {
      // https://man7.org/linux/man-pages/man3/perror.3.html
      perror("Connect error - no server available");
      return EXIT_FAILURE;
   }

   // ignore return value of printf
   printf("Connection with server (%s) established\n",
          inet_ntoa(address.sin_addr));

   ////////////////////////////////////////////////////////////////////////////
   // RECEIVE DATA
   // https://man7.org/linux/man-pages/man2/recv.2.html
   size = recv(create_socket, buffer, BUF - 1, 0);
   if (size == -1)
   {
      perror("recv error");
   }
   else if (size == 0)
   {
      printf("Server closed remote socket\n"); // ignore error
   }
   else
   {
      buffer[size] = '\0';
      printf("%s", buffer); // ignore error
   }

   do
   {
      printf(">> ");
      if (fgets(buffer, BUF - 1, stdin) != NULL)
      {
         int size = strlen(buffer);
         // remove new-line signs from string at the end
         if (buffer[size - 2] == '\r' && buffer[size - 1] == '\n')
         {
            size -= 2;
            buffer[size] = 0;
         }
         else if (buffer[size - 1] == '\n')
         {
            --size;
            buffer[size] = 0;
         }
         
         isQuit = strcmp(buffer, "QUIT") == 0;

         //////////////////////////////////////////////////////////////////////
         // Check if SEND command
         if (strcmp(buffer, "SEND") == 0)
         {
            // Handle SEND command specially
            if (handleSendCommand(create_socket) == -1)
            {
               fprintf(stderr, "<< SEND command failed\n");
            }
            continue; // Skip normal command processing
         }

         //////////////////////////////////////////////////////////////////////
         // SEND DATA
         // https://man7.org/linux/man-pages/man2/send.2.html
         // send will fail if connection is closed, but does not set
         // the error of send, but still the count of bytes sent
         if ((send(create_socket, buffer, size + 1, 0)) == -1) 
         {
            // in case the server is gone offline we will still not enter
            // this part of code: see docs: https://linux.die.net/man/3/send
            // >> Successful completion of a call to send() does not guarantee 
            // >> delivery of the message. A return value of -1 indicates only 
            // >> locally-detected errors.
            // ... but
            // to check the connection before send is sense-less because
            // after checking the communication can fail (so we would need
            // to have 1 atomic operation to check...)
            perror("send error");
            break;
         }

         //////////////////////////////////////////////////////////////////////
         // RECEIVE FEEDBACK
         // consider: reconnect handling might be appropriate in somes cases
         //           How can we determine that the command sent was received 
         //           or not? 
         //           - Resend, might change state too often. 
         //           - Else a command might have been lost.
         //
         // solution 1: adding meta-data (unique command id) and check on the
         //             server if already processed.
         // solution 2: add an infrastructure component for messaging (broker)
         //
         size = recv(create_socket, buffer, BUF - 1, 0);
         if (size == -1)
         {
            perror("recv error");
            break;
         }
         else if (size == 0)
         {
            printf("Server closed remote socket\n"); // ignore error
            break;
         }
         else
         {
            buffer[size] = '\0';
            printf("<< %s", buffer); // ignore error
         }
      }
   } while (!isQuit);

   ////////////////////////////////////////////////////////////////////////////
   // CLOSES THE DESCRIPTOR
   if (create_socket != -1)
   {
      if (shutdown(create_socket, SHUT_RDWR) == -1)
      {
         // invalid in case the server is gone already
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

// Verarbeitet den SEND command zwischen  user und server
int handleSendCommand(int socket)
{
   char buffer[BUF];
   char username[9];
   char subject[81];
   char line[BUF];
   int size;

   // überprüft ob socket gültig ist
   if (send(socket, "SEND\n", 5, 0) == -1)
   {
      perror("send SEND command failed");
      return -1;
   }

   // schickt username des empfängers
   printf("Username (max 8 characters): ");
   if (fgets(username, sizeof(username), stdin) == NULL)
   {
      fprintf(stderr, "Error reading username\n");
      return -1;
   }

   // löscht newline
   size = strlen(username);
   if (username[size - 1] == '\n')
   {
      username[size - 1] = '\0';
      size--;
   }

   // prüft Länge des username
   if (size == 0 || size > 8)
   {
      fprintf(stderr, "Invalid username length (must be 1-8 characters)\n");
      return -1;
   }

   // Sendet den username
   snprintf(buffer, sizeof(buffer), "%s\n", username);
   if (send(socket, buffer, strlen(buffer), 0) == -1)
   {
      perror("send username failed");
      return -1;
   }

   // betreff vom user
   printf("Subject (max 80 characters): ");
   if (fgets(subject, sizeof(subject), stdin) == NULL)
   {
      fprintf(stderr, "Error reading subject\n");
      return -1;
   }

   // löscht newline
   size = strlen(subject);
   if (subject[size - 1] == '\n')
   {
      subject[size - 1] = '\0';
      size--;
   }

   // prüft Länge des betreffs
   if (size == 0 || size > 80)
   {
      fprintf(stderr, "Invalid subject length (must be 1-80 characters)\n");
      return -1;
   }

   // Send betreff
   snprintf(buffer, sizeof(buffer), "%s\n", subject);
   if (send(socket, buffer, strlen(buffer), 0) == -1)
   {
      perror("send subject failed");
      return -1;
   }

   // message vom user
   printf("Message (end with a line containing only '.'):\n");
   
   while (1)
   {
      if (fgets(line, sizeof(line), stdin) == NULL)
      {
         fprintf(stderr, "Error reading message\n");
         return -1;
      }

      // Checkt auf end marker und zeilenumbruch
      if (strcmp(line, ".\n") == 0 || strcmp(line, ".") == 0)
      {
         // sendet den end marker
         if (send(socket, ".\n", 2, 0) == -1)
         {
            perror("send end marker failed");
            return -1;
         }
         break;
      }
      // Sendet Nachricht als zeile
      if (send(socket, line, strlen(line), 0) == -1)
      {
         perror("send message line failed");
         return -1;
      }
   }

   // Antwort vom Server abwarten
   size = recv(socket, buffer, BUF - 1, 0);
   if (size == -1)
   {
      perror("recv response failed");
      return -1;
   }
   else if (size == 0)
   {
      printf("Server closed connection\n");
      return -1;
   }
   else
   {
      buffer[size] = '\0';
      printf("<< %s", buffer);
      
      // überprüfen auf error 
      if (strncmp(buffer, "OK", 2) == 0)
      {
         return 0; // Success
      }
      else
      {
         return -1; // Error
      }
   }
}
