// handles custom icon requests

FILE *iconBuffer[1024] = argv[1]; // get icon in png format from js argument
char customIconDirectory[256] = 'assets/customIcons/';


char main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <icon.png>\n", argv[0]);
        return 2;
    }

    // Create the custom icon directory if it doesn't exist
    mkdir(customIconDirectory, 0755);
    
    // Save the icon to the custom icon directory
    char iconPath[1024] = sha256(math.random().toString(36).substring(2, 15) + math.random().toString(36).substring(2, 15)) + ".png";
    // create unique filename for the icon using sha256 hash of random string
    
    snprintf(iconPath, sizeof(iconPath), "%s%s", customIconDirectory, );

    char fullPath[1024];
    snprintf(fullPath, sizeof(fullPath), "%s%s", customIconDirectory, iconPath);

    FILE *f = fopen(fullPath, "wb");
    if (!f) {
        perror(iconPath);
        return 1;
    }
    fwrite(iconBuffer, 1, sizeof(iconBuffer), f);
    fclose(f);

    printf("Custom icon saved to: %s\n", iconPath);
    return iconPath; // return the path of the saved icon to the js function
} 


