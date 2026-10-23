// this program handles the backend of the account settings page

userToken = argv[1]; // get user token from js argument
userID = argv[2]; // get user id from js argument

FILE *f = fopen("accdta/${userID}.txt", "r"); // open the user's account data file

if (!f) {
    fprintf(stderr, "Error: Unable to open account data file for user %s\n", userID);
    return 1;
}   

char username[128]; 
char email[256];
int phoneNumber;
char securityPhrase[64];
char premiumStatus[16];
char customIcon[256];
int cardNumber;
int cardExpMonth;
int cardExpYear;
// do not store the card cvv in the account data file, it is not needed and should not be stored for security reasons

fscanf(f, "username: %127s\n", username);// line 2 each item is on a particular line in the file, so we can use fscanf to read them
fscanf(f, "email: %255s\n", email); // line 6
fscanf(f, "phone_number: %d\n", &phoneNumber); // line 7
fscanf(f, "security_phrase: %63s\n", securityPhrase);  // line 12
fscanf(f, "premium_status: %15s\n", premiumStatus);  // line 4
fscanf(f, "custom_icon: %255s\n", customIcon);  // line 8
fscanf(f, "card_number: %d\n", &cardNumber);  // not in file format yet, but will be added in the future, so we can read it now
fscanf(f, "card_exp_month: %d\n", &cardExpMonth);
fscanf(f, "card_exp_year: %d\n", &cardExpYear);  
fclose(f); // close the file


struct json_object *jobj = json_object_new_object();
json_object_object_add(jobj, "username", json_object_new_string(username));
json_object_object_add(jobj, "email", json_object_new_string(email));
json_object_object_add(jobj, "phone_number", json_object_new_int(phoneNumber));
json_object_object_add(jobj, "security_phrase", json_object_new_string(securityPhrase));
json_object_object_add(jobj, "premium_status", json_object_new_string(premiumStatus));
json_object_object_add(jobj, "custom_icon", json_object_new_string(customIcon));
json_object_object_add(jobj, "card_number", json_object_new_int(cardNumber));
json_object_object_add(jobj, "card_exp_month", json_object_new_int(cardExpMonth));
json_object_object_add(jobj, "card_exp_year", json_object_new_int(cardExpYear));    


printf("%s\n", json_object_to_json_string(jobj)); // print the json object to stdout so it can be read by the js function



// if pairs of data is sent to this function, it will update the user's account data file with the new data, first peice isto change, second piece is new data
// do not use this function to change icon there is a separate function for that
// the users token is aoutomatic and cannot be changed
// the user id id permanent and cannot be changed, this is because users can change thier username and email, but the user id is permanent and cannot be changed, so it is used to identify the user in the account data file
int numArgs = argc - 2; // number of arguments after the user id
if (numArgs % 2 != 0) {
    fprintf(stderr, "Error: Invalid number of arguments for updating account data\n");
    return 1;
}   

if (argc > 3) {
    for (int i = 3; i < argc; i += 2) {
        const char *field = argv[i];
        const char *newValue = argv[i + 1];

        if (strcmp(field, "username") == 0) {
            strncpy(username, newValue, sizeof(username) - 1);
            username[sizeof(username) - 1] = '\0';
        } else if (strcmp(field, "email") == 0) {
            strncpy(email, newValue, sizeof(email) - 1);
            email[sizeof(email) - 1] = '\0';
        } else if (strcmp(field, "phone_number") == 0) {
            phoneNumber = atoi(newValue);
        } else if (strcmp(field, "security_phrase") == 0) {
            strncpy(securityPhrase, newValue, sizeof(securityPhrase) - 1);
            securityPhrase[sizeof(securityPhrase) - 1] = '\0';
        } else if (strcmp(field, "premium_status") == 0) {
            strncpy(premiumStatus, newValue, sizeof(premiumStatus) - 1);
            premiumStatus[sizeof(premiumStatus) - 1] = '\0';
        } else if (strcmp(field, "custom_icon") == 0) {
            strncpy(customIcon, newValue, sizeof(customIcon) - 1);
            customIcon[sizeof(customIcon) - 1] = '\0';
        } else {
            fprintf(stderr, "Error: Unknown field '%s' for updating account data\n", field);
            return 1;
        }
    }
    f = fopen("accdta/${userID}.txt", "w");
    if (!f) {
        fprintf(stderr, "Error: Unable to open account data file for writing for user %s\n", userID);
        return 1;
    }

    fprintf(f, "username: %s\n", username);
    fprintf(f, "email: %s\n", email);
    fprintf(f, "phone_number: %d\n", phoneNumber);
    fprintf(f, "security_phrase: %s\n", securityPhrase);
    fprintf(f, "premium_status: %s\n", premiumStatus);
    fprintf(f, "card_number: %d\n", cardNumber);
    fprintf(f, "card_exp_month: %d\n", cardExpMonth);
    fprintf(f, "card_exp_year: %d\n", cardExpYear);
    fprintf(f, "custom_icon: %s\n", customIcon);

    fclose(f);
}