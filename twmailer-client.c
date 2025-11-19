#define _DEFAULT_SOURCE

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <termios.h>

///////////////////////////////////////////////////////////////////////////////

#define BUF 1024

///////////////////////////////////////////////////////////////////////////////

int handleLoginCommand(int socket);
int handleSendCommand(int socket);
int handleListCommand(int socket);
int handleReadCommand(int socket);
int handleDelCommand(int socket);
ssize_t readline(int fd, void *vptr, size_t maxlen);
int isValidUsername(const char *username);
int getch();
void getpass_masked(char *password, size_t maxlen);

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
   size = readline(create_socket, buffer, BUF - 1);
   if (size == -1)
   {
      perror("readline error");
   }
   else if (size == 0)
   {
      printf("Server closed remote socket\n"); // ignore error
   }
   else
   {
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

         // Check if LOGIN command
         if (strcmp(buffer, "LOGIN") == 0)
         {
            if (handleLoginCommand(create_socket) == -1)
            {
               fprintf(stderr, "<< LOGIN command failed\n");
            }
            continue; 
         }

         // Check if SEND command
         if (strcmp(buffer, "SEND") == 0)
         {
            // Handle SEND command specially
            if (handleSendCommand(create_socket) == -1)
            {
               fprintf(stderr, "<< SEND command failed\n");
            }
            continue; 
         }

         // Check if LIST command
         if (strcmp(buffer, "LIST") == 0)
         {
            // Handle LIST command specially
            if (handleListCommand(create_socket) == -1)
            {
               fprintf(stderr, "<< LIST command failed\n");
            }
            continue; 
         }

         // Check if READ command
         if (strcmp(buffer, "READ") == 0)
         {
            if (handleReadCommand(create_socket) == -1)
            {
               fprintf(stderr, "<< READ command failed\n");
            }
            continue; 
         }

         // Check if DEL command
         if (strcmp(buffer, "DEL") == 0)
         {
            if (handleDelCommand(create_socket) == -1)
            {
               fprintf(stderr, "<< DEL command failed\n");
            }
            continue; 
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
         size = readline(create_socket, buffer, BUF - 1);
         if (size == -1)
         {
            perror("readline error");
            break;
         }
         else if (size == 0)
         {
            printf("Server closed remote socket\n"); // ignore error
            break;
         }
         else
         {
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

// LOGIN command handler
int handleLoginCommand(int socket)
{
   char buffer[BUF];
   char username[128];
   char password[256];
   int size;

   // Send LOGIN command
   if (send(socket, "LOGIN\n", 6, 0) == -1)
   {
      perror("send LOGIN command failed");
      return -1;
   }

   // Get username
   printf("LDAP Username: ");
   if (fgets(username, sizeof(username), stdin) == NULL)
   {
      fprintf(stderr, "Error reading username\n");
      return -1;
   }

   // Remove newline
   size = strlen(username);
   if (username[size - 1] == '\n')
   {
      username[size - 1] = '\0';
      size--;
   }

   if (size == 0)
   {
      fprintf(stderr, "Username cannot be empty\n");
      return -1;
   }

   // Send username
   snprintf(buffer, sizeof(buffer), "%s\n", username);
   if (send(socket, buffer, strlen(buffer), 0) == -1)
   {
      perror("send username failed");
      return -1;
   }

      // Get password mit Masking
   getpass_masked(password, sizeof(password));
      
   if (strlen(password) == 0)
   {
      fprintf(stderr, "Password cannot be empty\n");
      return -1;
   }

   // Send password
   snprintf(buffer, sizeof(buffer), "%s\n", password);
   if (send(socket, buffer, strlen(buffer), 0) == -1)
   {
      perror("send password failed");
      return -1;
   }

   // Receive response
   size = readline(socket, buffer, BUF - 1);
   if (size == -1)
   {
      perror("readline response failed");
      return -1;
   }
   else if (size == 0)
   {
      printf("Server closed connection\n");
      return -1;
   }

   printf("<< %s", buffer);

   if (strncmp(buffer, "OK", 2) == 0)
   {
      printf("Login successful!\n");
      return 0;
   }
   else
   {
      printf("Login failed!\n");
      return -1;
   }
}

// Verarbeitet den SEND command zwischen  user und server
int handleSendCommand(int socket)
{
   char buffer[BUF];
   char receiver[9];
   char subject[81];
   char line[BUF];
   int size;

   // überprüft ob socket gültig ist
   if (send(socket, "SEND\n", 5, 0) == -1)
   {
      perror("send SEND command failed");
      return -1;
   }

   // Sender wird automatisch aus Session genommen - nicht mehr eingeben
   printf("(Sender will be set from your login session)\n");

   // Receiver eingeben
   printf("Receiver (max 8 characters): ");
   if (fgets(receiver, sizeof(receiver), stdin) == NULL)
   {
      fprintf(stderr, "Error reading receiver\n");
      return -1;
   }

   // löscht newline und leert buffer wenn input zu lang war
   size = strlen(receiver);
   if (receiver[size - 1] == '\n')
   {
      receiver[size - 1] = '\0';
      size--;
   }
   else
   {
      // Input war zu lang, buffer leeren
      int c;
      while ((c = getchar()) != '\n' && c != EOF)
         ;
   }

   // prüft Länge des receiver
   if (size == 0 || size > 8)
   {
      fprintf(stderr, "Invalid receiver length (must be 1-8 characters)\n");
      return -1;
   }

   // prüft ob username nur a-z und 0-9 enthält
   if (!isValidUsername(receiver))
   {
      fprintf(stderr, "Invalid receiver: only lowercase letters (a-z) and digits (0-9) allowed\n");
      return -1;
   }

   // Sendet den receiver
   snprintf(buffer, sizeof(buffer), "%s\n", receiver);
   if (send(socket, buffer, strlen(buffer), 0) == -1)
   {
      perror("send receiver failed");
      return -1;
   }

   // betreff vom user
   printf("Subject (max 80 characters): ");
   if (fgets(subject, sizeof(subject), stdin) == NULL)
   {
      fprintf(stderr, "Error reading subject\n");
      return -1;
   }

   // löscht newline und leert buffer wenn input zu lang war
   size = strlen(subject);
   if (subject[size - 1] == '\n')
   {
      subject[size - 1] = '\0';
      size--;
   }
   else
   {
      // buffer leeren wenn nachricht zu lang
      int c;
      while ((c = getchar()) != '\n' && c != EOF)
         ;
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
   size = readline(socket, buffer, BUF - 1);
   if (size == -1)
   {
      perror("readline response failed");
      return -1;
   }
   else if (size == 0)
   {
      printf("Server closed connection\n");
      return -1;
   }
   else
   {
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

// Funktion um den LIST command zu verarbeiten
// Pro Version: Username wird aus Session genommen
// Format:
// LIST
//
// Response:
// count
// subject1
// subject2
int handleListCommand(int socket)
{
   char buffer[BUF];
   int size;

   // Send LIST command
   if (send(socket, "LIST\n", 5, 0) == -1)
   {
      perror("send LIST command failed");
      return -1;
   }

   // Username wird automatisch aus Session genommen
   printf("(Listing messages for your logged-in account)\n");

   // bekommt count der nachrichten
   size = readline(socket, buffer, BUF - 1);
   if (size == -1)
   {
      perror("readline count failed");
      return -1;
   }
   else if (size == 0)
   {
      printf("Server closed connection\n");
      return -1;
   }

   // wandelt count in integer um
   int messageCount = atoi(buffer);
   printf("<< %d message(s)\n", messageCount);

   // prüft ob es noch nachrichten gibt
   if (messageCount == 0)
   {
      return 0;
   }

   // subjects zeilenweise empfangen und anzeigen
   for (int i = 1; i <= messageCount; i++)
   {
      size = readline(socket, buffer, BUF - 1);
      if (size <= 0)
      {
         break;
      }
      // Remove newline
      if (size > 0 && buffer[size - 1] == '\n')
      {
         buffer[size - 1] = '\0';
      }
      printf("  %d. %s\n", i, buffer);
   }

   return 0;
}

// READ command handler
// Pro Version: Username wird aus Session genommen
int handleReadCommand(int socket)
{
   char buffer[BUF];
   char messageNum[10];
   int size;

   // Send READ command
   if (send(socket, "READ\n", 5, 0) == -1)
   {
      perror("send READ command failed");
      return -1;
   }

   printf("(Reading message from your logged-in account)\n");

   // Get message number
   printf("Message number: ");
   if (fgets(messageNum, sizeof(messageNum), stdin) == NULL)
   {
      fprintf(stderr, "Error reading message number\n");
      return -1;
   }

   // Remove newline
   size = strlen(messageNum);
   if (messageNum[size - 1] == '\n')
   {
      messageNum[size - 1] = '\0';
   }

   // Send message number
   snprintf(buffer, sizeof(buffer), "%s\n", messageNum);
   if (send(socket, buffer, strlen(buffer), 0) == -1)
   {
      perror("send message number failed");
      return -1;
   }

   // Receive response
   size = readline(socket, buffer, BUF - 1);
   if (size == -1)
   {
      perror("readline response failed");
      return -1;
   }
   else if (size == 0)
   {
      printf("Server closed connection\n");
      return -1;
   }

   // Check if OK or ERR
   if (strncmp(buffer, "ERR", 3) == 0)
   {
      printf("<< %s", buffer);
      return -1;
   }

   // Print OK line
   printf("<< %s", buffer);

   // empfängt nachricht zeilenweise bis zum end marker
   while (1)
   {
      size = readline(socket, buffer, BUF - 1);
      if (size <= 0)
      {
         break;
      }

      // end marker check
      if (strcmp(buffer, ".\n") == 0 || strcmp(buffer, ".") == 0)
      {
         break;
      }

      printf("%s", buffer);
   }

   return 0;
}

// DEL command handler
int handleDelCommand(int socket)
{
   char buffer[BUF];
   char messageNum[10];
   int size;

   // Send DEL command
   if (send(socket, "DEL\n", 4, 0) == -1)
   {
      perror("send DEL command failed");
      return -1;
   }

   printf("(Deleting message from your logged-in account)\n");

   // Get message number
   printf("Message number: ");
   if (fgets(messageNum, sizeof(messageNum), stdin) == NULL)
   {
      fprintf(stderr, "Error reading message number\n");
      return -1;
   }

   // Remove newline
   size = strlen(messageNum);
   if (messageNum[size - 1] == '\n')
   {
      messageNum[size - 1] = '\0';
   }

   // Send message number
   snprintf(buffer, sizeof(buffer), "%s\n", messageNum);
   if (send(socket, buffer, strlen(buffer), 0) == -1)
   {
      perror("send message number failed");
      return -1;
   }

   // Receive response
   size = readline(socket, buffer, BUF - 1);
   if (size == -1)
   {
      perror("readline response failed");
      return -1;
   }
   else if (size == 0)
   {
      printf("Server closed connection\n");
      return -1;
   }

   // Print response
   printf("<< %s", buffer);

   // Check if OK or ERR
   if (strncmp(buffer, "OK", 2) == 0)
   {
      return 0; // Success
   }
   else
   {
      return -1; // Error
   }
}

// readline() - Stevens Implementation from PDF
ssize_t readline(int fd, void *vptr, size_t maxlen)
{
   ssize_t n, rc;
   char c, *ptr;

   ptr = vptr;
   for (n = 1; n < maxlen; n++)
   {
   again:
      if ((rc = read(fd, &c, 1)) == 1)
      {
         *ptr++ = c;
         if (c == '\n')
            break; // newline is stored, like fgets()
      }
      else if (rc == 0)
      {
         if (n == 1)
            return (0); // EOF, no data read
         else
            break; // EOF, some data was read
      }
      else
      {
         if (errno == EINTR)
            goto again;
         return (-1); // error, errno set by read()
      }
   }

   *ptr = 0; // null terminate like fgets()
   return (n);
}

// Validiert Username: nur a-z und 0-9 erlaubt
int isValidUsername(const char *username)
{
   if (username == NULL || *username == '\0')
   {
      return 0; // leer
   }

   for (int i = 0; username[i] != '\0'; i++)
   {
      char c = username[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
      {
         return 0; // ungültiges Zeichen
      }
   }

   return 1; // valid
}

//um einen char ohne eingabe zu lesen
int getch()
{
   int ch;
   struct termios t_old, t_new;
   tcgetattr(STDIN_FILENO, &t_old);
   t_new = t_old;
   t_new.c_lflag &= ~(ICANON | ECHO);
   tcsetattr(STDIN_FILENO, TCSANOW, &t_new);
   ch = getchar();
   tcsetattr(STDIN_FILENO, TCSANOW, &t_old);
   return ch;
}

// Passwort Eingabe mit Maskierung
void getpass_masked(char *password, size_t maxlen)
{
   const char BACKSPACE = 127;
   const char RETURN = 10;
   unsigned char ch;
   size_t len = 0;

   printf("Password: ");
   fflush(stdout);

   // Eingabe lesen und mit '*' maskieren
   while ((ch = getch()) != RETURN && len < maxlen - 1)
   {
      if (ch == BACKSPACE)
      {
         if (len > 0)
         {
            printf("\b \b");
            fflush(stdout);
            len--;
         }
      }
      else
      {
         password[len++] = ch; // speichert das Zeichen
         printf("*");   // schreibt '*' statt des Zeichens
         fflush(stdout);
      }
   }
   password[len] = '\0';
   printf("\n");
}