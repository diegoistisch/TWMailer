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
#include <ldap.h>

///////////////////////////////////////////////////////////////////////////////

#define BUF 1024

///////////////////////////////////////////////////////////////////////////////

int abortRequested = 0;
int create_socket = -1;
int new_socket = -1;
char *mailSpoolDir = NULL;

// Session-Daten (wird pro Client-Verbindung gesetzt)
int isAuthenticated = 0;
char sessionUsername[256]; // LDAP-Username nach Login

///////////////////////////////////////////////////////////////////////////////

void *clientCommunication(void *data);
void signalHandler(int sig);
int handleLogin(int socket);
int handleSend(int socket);
int handleList(int socket);
int handleRead(int socket);
int handleDel(int socket);
int getNextMessageNumber(const char *userDir);
ssize_t readline(int fd, void *buffer, size_t n);
int isValidUsername(const char *username);

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

   // Session zurücksetzen für neue Verbindung
   isAuthenticated = 0;
   memset(sessionUsername, 0, sizeof(sessionUsername));

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
      if (strcmp(buffer, "LOGIN") == 0)
      {
         if (handleLogin(*current_socket) == -1)
         {
            if (send(*current_socket, "ERR\n", 4, 0) == -1)
            {
               perror("send error response failed");
            }
         }
      }
      else if (strcmp(buffer, "SEND") == 0)
      {
         if (!isAuthenticated) // prüft ob user eingeloggt ist
         {
            printf("SEND rejected - not authenticated\n"); 
            if (send(*current_socket, "ERR\n", 4, 0) == -1) 
            {
               perror("send error response failed");
            }
         }
         else if (handleSend(*current_socket) == -1) // prüft ob SEND erfolgreich war
         {
            if (send(*current_socket, "ERR\n", 4, 0) == -1)
            {
               perror("send error response failed");
            }
         }
      }
      else if (strcmp(buffer, "LIST") == 0)
      {
         if (!isAuthenticated)
         {
            printf("LIST rejected - not authenticated\n");
            if (send(*current_socket, "ERR\n", 4, 0) == -1)
            {
               perror("send error response failed");
            }
         }
         else if (handleList(*current_socket) == -1)
         {
            if (send(*current_socket, "ERR\n", 4, 0) == -1)
            {
               perror("send error response failed");
            }
         }
      }
      else if (strcmp(buffer, "READ") == 0)
      {
         if (!isAuthenticated)
         {
            printf("READ rejected - not authenticated\n");
            if (send(*current_socket, "ERR\n", 4, 0) == -1)
            {
               perror("send error response failed");
            }
         }
         else if (handleRead(*current_socket) == -1)
         {
            if (send(*current_socket, "ERR\n", 4, 0) == -1)
            {
               perror("send error response failed");
            }
         }
      }
      else if (strcmp(buffer, "DEL") == 0)
      {
         if (!isAuthenticated)
         {
            printf("DEL rejected - not authenticated\n");
            if (send(*current_socket, "ERR\n", 4, 0) == -1)
            {
               perror("send error response failed");
            }
         }
         else if (handleDel(*current_socket) == -1)
         {
            if (send(*current_socket, "ERR\n", 4, 0) == -1)
            {
               perror("send error response failed");
            }
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

   // verbindung schließen 
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

// LOGIN command handler mit LDAP-Authentifizierung
int handleLogin(int socket)
{
   char buffer[BUF];
   char ldapUsername[128];
   char ldapPassword[256];
   int size;

   // LDAP Konfiguration
   const char *ldapUri = "ldap://ldap.technikum-wien.at:389";
   const int ldapVersion = LDAP_VERSION3;
   char ldapBindUser[256];

   // Empfange Username
   size = readline(socket, buffer, BUF - 1);
   if (size <= 0)
   {
      perror("readline username failed");
      return -1;
   }

   // Remove newline
   if (size > 0 && buffer[size - 1] == '\n')
   {
      buffer[size - 1] = '\0';
      size--;
   }

   if (size == 0 || size > 127)
   {
      fprintf(stderr, "Invalid username length\n");
      return -1;
   }
   
   //username in ldap username buffer
   strncpy(ldapUsername, buffer, sizeof(ldapUsername) - 1); 
   ldapUsername[sizeof(ldapUsername) - 1] = '\0';
   printf("LOGIN attempt for user: %s\n", ldapUsername);

   // Empfange Password
   size = readline(socket, buffer, BUF - 1);
   if (size <= 0)
   {
      perror("readline password failed");
      return -1;
   }

   // Remove newline
   if (size > 0 && buffer[size - 1] == '\n')
   {
      buffer[size - 1] = '\0';
      size--;
   }

   // Passwort in ldap password buffer speichern
   strncpy(ldapPassword, buffer, sizeof(ldapPassword) - 1);
   ldapPassword[sizeof(ldapPassword) - 1] = '\0';

   // LDAP Bind User DN erstellen
   sprintf(ldapBindUser, "uid=%s,ou=people,dc=technikum-wien,dc=at", ldapUsername);
   printf("LDAP bind DN: %s\n", ldapBindUser);

   // LDAP Verbindung aufbauen
   LDAP *ldapHandle;
   int rc = ldap_initialize(&ldapHandle, ldapUri);
   if (rc != LDAP_SUCCESS)
   {
      fprintf(stderr, "ldap_initialize failed: %s\n", ldap_err2string(rc));
      return -1;
   }
   printf("Connected to LDAP server %s\n", ldapUri);

   // LDAP Version setzen
   rc = ldap_set_option(ldapHandle, LDAP_OPT_PROTOCOL_VERSION, &ldapVersion);
   if (rc != LDAP_OPT_SUCCESS)
   {
      fprintf(stderr, "ldap_set_option(PROTOCOL_VERSION): %s\n", ldap_err2string(rc));
      ldap_unbind_ext_s(ldapHandle, NULL, NULL);
      return -1;
   }

   
   // TLS starten
   rc = ldap_start_tls_s(ldapHandle, NULL, NULL);
   if (rc != LDAP_SUCCESS)
   {
      fprintf(stderr, "ldap_start_tls_s(): %s\n", ldap_err2string(rc));
      ldap_unbind_ext_s(ldapHandle, NULL, NULL);
      return -1;
   }

   // LDAP Bind (Authentifizierung)
   BerValue bindCredentials;
   bindCredentials.bv_val = (char *)ldapPassword;
   bindCredentials.bv_len = strlen(ldapPassword);
   BerValue *servercredp;

   rc = ldap_sasl_bind_s(
       ldapHandle,
       ldapBindUser,
       LDAP_SASL_SIMPLE,
       &bindCredentials,
       NULL,
       NULL,
       &servercredp);

   if (rc != LDAP_SUCCESS)
   {
      fprintf(stderr, "LDAP bind error: %s\n", ldap_err2string(rc));
      ldap_unbind_ext_s(ldapHandle, NULL, NULL);
      return -1;
   }

   // Authentifizierung erfolgreich!
   printf("LDAP authentication successful for user: %s\n", ldapUsername);
   ldap_unbind_ext_s(ldapHandle, NULL, NULL);

   // Session-Daten setzen
   isAuthenticated = 1;
   strncpy(sessionUsername, ldapUsername, sizeof(sessionUsername) - 1);
   sessionUsername[sizeof(sessionUsername) - 1] = '\0';

   // Sende OK
   if (send(socket, "OK\n", 3, 0) == -1)
   {
      perror("send OK failed");
      return -1;
   }

   return 0;
}

// funktion um den SEND command zu verarbeiten
// format (Pro Version):
// SEND
// <Receiver>
// <Subject>
// <message>
// .
// Sender wird automatisch aus Session gesetzt
int handleSend(int socket)
{
   char buffer[BUF];
   char username[9];       // Max 8 characters + null terminator
   char subject[81];       // Max 80 characters + null terminator
   char message[BUF * 10]; // um längere nachrichten zu erlauben
   char userDir[256];
   char filePath[300];
   int size;
   FILE *file;
   int messageNum;

   memset(message, 0, sizeof(message));

   // Sender wird automatisch aus Session genommen
   printf("Sender (from session): %s\n", sessionUsername);

   // Receive receiver (username)
   size = readline(socket, buffer, BUF - 1);
   if (size <= 0)
   {
      perror("readline receiver failed");
      return -1;
   }

   // Remove newline
   if (size > 0 && buffer[size - 1] == '\n')
   {
      buffer[size - 1] = '\0';
      size--;
   }

   // Validate receiver (max 8 characters)
   if (size > 8 || size == 0)
   {
      fprintf(stderr, "Invalid receiver length: %d\n", size);
      return -1;
   }
   strncpy(username, buffer, sizeof(username) - 1);
   username[sizeof(username) - 1] = '\0';

   // prüft ob receiver nur a-z und 0-9 enthält
   if (!isValidUsername(username))
   {
      fprintf(stderr, "Invalid receiver: only lowercase letters (a-z) and digits (0-9) allowed\n");
      return -1;
   }

   printf("Receiver: %s\n", username);

   // Receive subject
   size = readline(socket, buffer, BUF - 1);
   if (size <= 0)
   {
      perror("readline subject failed");
      return -1;
   }

   // Remove newline
   if (size > 0 && buffer[size - 1] == '\n')
   {
      buffer[size - 1] = '\0';
      size--;
   }

   // Validate subject (max 80 characters)
   if (size > 80 || size == 0)
   {
      fprintf(stderr, "Invalid subject length: %d\n", size);
      return -1;
   }
   strncpy(subject, buffer, sizeof(subject) - 1);
   subject[sizeof(subject) - 1] = '\0';
   printf("Subject: %s\n", subject);

   // empfängt nachrichten
   int messageLen = 0;
   while (1)
   {
      size = readline(socket, buffer, BUF - 1);
      if (size <= 0)
      {
         perror("readline message failed");
         return -1;
      }

      // Remove newline
      if (size > 0 && buffer[size - 1] == '\n')
      {
         buffer[size - 1] = '\0';
         size--;
      }

      // Checkt für den End Marker
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

   // formatiert nachricht im file: sender (aus session), receiver, subject, message
   fprintf(file, "%s\n%s\n%s\n%s\n", sessionUsername, username, subject, message);
   fclose(file);

   printf("Message saved to: %s\n", filePath);

   if (send(socket, "OK\n", 3, 0) == -1)
   {
      perror("send OK failed");
      return -1;
   }

   return 0;
}

// Funktion um den LIST command zu verarbeiten
// Format (Pro Version):
// LIST
// (kein username mehr, wird aus Session genommen)
//
// Response:
// count
// subject1
// subject2
int handleList(int socket)
{
   char userDir[512];
   char filePath[1024];
   DIR *dir;
   struct dirent *entry;
   int messageCount = 0;
   char response[BUF * 10];
   int responseLen = 0;

   memset(response, 0, sizeof(response));

   // Username wird aus Session genommen
   printf("LIST command for user (from session): %s\n", sessionUsername);

   // Verzeichnis erstellen
   snprintf(userDir, sizeof(userDir), "%s/%s", mailSpoolDir, sessionUsername);

   // Verzeichnis öffnen
   dir = opendir(userDir);
   if (dir == NULL)
   {
      // Benutzerverzeichnis existiert nicht, 0 Nachrichten zurückgeben
      printf("User directory not found, returning 0 messages\n");
      if (send(socket, "0\n", 2, 0) == -1)
      {
         perror("send 0 count failed");
         return -1;
      }
      return 0;
   }

   // Liest alle .txt Dateien und zählt sie
   while ((entry = readdir(dir)) != NULL)
   {
      if (strstr(entry->d_name, ".txt") != NULL)
      {
         messageCount++;
      }
   }
   closedir(dir);

   printf("Found %d messages for user %s\n", messageCount, sessionUsername);

   // erstellt response mit count
   responseLen = snprintf(response, sizeof(response), "%d\n", messageCount);

   // öffent verzeichnis erneut um subjects zu lesen
   dir = opendir(userDir);
   if (dir == NULL)
   {
      perror("re-opendir failed");
      return -1;
   }

   // liest alle .txt dateien und extrahiert subjects
   while ((entry = readdir(dir)) != NULL)
   {
      // Überprüfen ob es sich um eine .txt Datei handelt
      if (strstr(entry->d_name, ".txt") != NULL)
      {
         // file path
         snprintf(filePath, sizeof(filePath), "%s/%s", userDir, entry->d_name);

         // öffnet datei und liest subject (dritte zeile)
         FILE *file = fopen(filePath, "r");
         if (file != NULL)
         {
            char line[BUF];
            // skip erste zeile (sender)
            if (fgets(line, sizeof(line), file) != NULL)
            {
               // skip zweite zeile (receiver)
               if (fgets(line, sizeof(line), file) != NULL)
               {
                  // liest dritte zeile (subject)
                  if (fgets(line, sizeof(line), file) != NULL)
                  {
                     // newLine entfernen
                     size_t len = strlen(line);
                     if (len > 0 && line[len - 1] == '\n')
                     {
                        line[len - 1] = '\0';
                     }

                     // subject zur response hinzufügen
                     int addLen = snprintf(response + responseLen,
                                           sizeof(response) - responseLen,
                                           "%s\n", line);
                     responseLen += addLen;
                  }
               }
            }
            fclose(file);
         }
      }
   }
   closedir(dir);

   // Send response
   if (send(socket, response, responseLen, 0) == -1)
   {
      perror("send LIST response failed");
      return -1;
   }

   printf("LIST response sent (%d bytes)\n", responseLen);
   return 0;
}

// READ command handler
// Format (Pro Version): READ\nmessage-number\n
// Username wird aus Session genommen
int handleRead(int socket)
{
   char buffer[BUF];
   char filePath[300];
   int messageNum;
   int size;
   FILE *file;
   char line[BUF];

   printf("READ command for user (from session): %s\n", sessionUsername);

   // Receive message number
   size = readline(socket, buffer, BUF - 1);
   if (size <= 0)
   {
      perror("readline message number failed");
      return -1;
   }

   // Remove newline
   if (size > 0 && buffer[size - 1] == '\n')
   {
      buffer[size - 1] = '\0';
      size--;
   }

   messageNum = atoi(buffer);
   if (messageNum <= 0)
   {
      fprintf(stderr, "Invalid message number: %s\n", buffer);
      return -1;
   }

   // Build file path mit sessionUsername
   snprintf(filePath, sizeof(filePath), "%s/%s/%d.txt", mailSpoolDir, sessionUsername, messageNum);

   // Open and read file
   file = fopen(filePath, "r");
   if (file == NULL)
   {
      perror("fopen failed");
      return -1;
   }

   // Send OK
   if (send(socket, "OK\n", 3, 0) == -1)
   {
      perror("send OK failed");
      fclose(file);
      return -1;
   }

   // Send file content line by line
   while (fgets(line, sizeof(line), file) != NULL)
   {
      if (send(socket, line, strlen(line), 0) == -1)
      {
         perror("send file content failed");
         fclose(file);
         return -1;
      }
   }

   // Schickt end marker
   if (send(socket, ".\n", 2, 0) == -1)
   {
      perror("send end marker failed");
      fclose(file);
      return -1;
   }

   fclose(file);
   printf("Message %d sent to client (user: %s)\n", messageNum, sessionUsername);
   return 0;
}

// DEL command handler
// Format (Pro Version): DEL\nmessage-number\n
// Username wird aus Session genommen
int handleDel(int socket)
{
   char buffer[BUF];
   char filePath[300];
   int messageNum;
   int size;

   printf("DEL command for user (from session): %s\n", sessionUsername);

   // lese message number
   size = readline(socket, buffer, BUF - 1);
   if (size <= 0)
   {
      perror("readline message number failed");
      return -1;
   }

   // Remove newline
   if (size > 0 && buffer[size - 1] == '\n')
   {
      buffer[size - 1] = '\0';
      size--;
   }

   messageNum = atoi(buffer);
   if (messageNum <= 0)
   {
      fprintf(stderr, "Invalid message number: %s\n", buffer);
      return -1;
   }

   printf("Attempting to delete message %d for user %s\n", messageNum, sessionUsername);

   // Build file path mit sessionUsername
   snprintf(filePath, sizeof(filePath), "%s/%s/%d.txt", mailSpoolDir, sessionUsername, messageNum);

   // Versuche die Datei zu löschen
   if (unlink(filePath) == -1)
   {
      perror("unlink failed - message not found or cannot be deleted");
      return -1;
   }

   printf("Message %d deleted successfully for user %s\n", messageNum, sessionUsername);

   // Send OK wenn es funktioniert hat
   if (send(socket, "OK\n", 3, 0) == -1)
   {
      perror("send OK failed");
      return -1;
   }

   return 0;
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
