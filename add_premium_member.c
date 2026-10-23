#define PREMIUM_ACTIVATION_TOOL
#include "premium_account_common.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s ACCOUNT_ID_OR_USERNAME MM-DD-YYYY\n", argv[0]);
        return EXIT_FAILURE;
    }

    struct tm expiry;
    if (!parse_premium_date(argv[2], &expiry)) {
        fprintf(stderr, "Invalid expiry date: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    char path[512];
    if (!find_account_path(argv[1], path, sizeof(path))) {
        fprintf(stderr, "Account not found: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (!update_premium_date(path, argv[2])) {
        fprintf(stderr, "Could not update premium status for %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    printf("Premium activated for %s through %s\n", argv[1], argv[2]);
    return EXIT_SUCCESS;
}