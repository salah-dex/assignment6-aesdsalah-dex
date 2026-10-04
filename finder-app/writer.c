#include <stdio.h>
#include <syslog.h>
#include <stdlib.h>
#include <errno.h>

int main(int argc,char* argv[])
{
/*1: initialize the LOG system */    
openlog(NULL,0,LOG_USER);

/*2: validate input args */   
   if (argc != 3) {
        syslog(LOG_ERR, "Usage: %s <file_path> <text_string>", argv[0]);
        return EXIT_FAILURE;
    }
    if( !argv[1] || !argv[2])
    {
        syslog(LOG_ERR, "Usage: %s <file_path> <text_string>", argv[0]);
        return EXIT_FAILURE;
    }
/*3: capture inputs */

    const char * file_path = argv[1];
    const char * text_string = argv[2];

    // Open the file for writing
    FILE *file = fopen(file_path, "w");
    if (file == NULL) {
        syslog(LOG_ERR, "Error opening file: %s", file_path);
        return EXIT_FAILURE;
    }

    // Write the string to the file
    fprintf(file, "%s\n", text_string);
    syslog(LOG_DEBUG, "Wrote string to file: %s", text_string);

    // Close the file
    fclose(file);
    return EXIT_SUCCESS;
}
