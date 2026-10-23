#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>


int main(int argc, char argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <claimor> <rfp_id> <status>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char claimor = argv[1];
    char rfp_id = argv[2];
    char status = argv[3];

std::string filepath = "./RFPs/" + status + "/" + rfp_id + ".txt"; // what is the correct way to concatenate strings in C? 
    

FILE *rfp_file = fopen(filepath.c_str(), "a");
    if (!rfp_file) {
        perror("Error opening RFP file");
        return EXIT_FAILURE;
    }

// add claimor id to the end of the file
if (rfp_file) {
    fprintf(rfp_file, "\nClaimor: %s", claimor);
    fclose(rfp_file);
}
};
