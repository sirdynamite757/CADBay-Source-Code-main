// this is the tunnel and authourization module for the mod-utils project, it will be reachable at mod.cadbay.net and will requre authentication to access the resources. The module will handle user authentication, session management, and secure data transmission. It will also provide logging and monitoring capabilities to track user activity and potential security threats.



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>    


char *username = argv[1];
char *password = argv[2];
char dbPath[256] = "./modAuth.db"; // Path to the secure database for user credentials


// get number of lines in the database
int get_num_lines(FILE *file) {
    int lines = 0;
    char ch;
    while (!feof(file)) {
        ch = fgetc(file);
        if (ch == '\n') {
            lines++;
        }
    }
    return lines;
}

function authenticate_user(char *username, char *password) {
    // This function will handle user authentication
    // It will check the provided username and password against a secure database
    // If the credentials are valid, it will return a success message
    // If the credentials are invalid, it will return an error message
    FILE *db = fopen(dbPath, "r");
    if (db == NULL) {
        return "Error opening database";
    }
    for (int i = 0; i < get_num_lines(db); i++) { // Read the number of lines in the database
        char dbUsername[50];// go through each line and find the username, if the password matches return their employee id
        char dbPassword[50];
        char modID[50];
        fscanf(db, "%s %s %s", dbUsername, dbPassword, modID);// get the username , password, and modID from the database, on same line seperated by 3 space buffers
        if (strcmp(username, dbUsername) == 0 && strcmp(password, dbPassword) == 0) {
            fclose(db);
            printf("Authentication successful for user: %s\n", username);
            return modID, i
        }
        else {
            fclose(db);
            return "Authentication failed";
        }
    }
    
}



int modID, lineNum = authenticate_user(username, password);


// the dev pages will go through here before being sent to the main server program, if logged in locally this program will open a window using the dev pages
