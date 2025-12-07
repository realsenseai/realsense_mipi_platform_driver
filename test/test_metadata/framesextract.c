#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRMDROPACCPTTHRESHOLD 30

void process_frame_data(void){
   // Extract BUFFER_IDX lines from frames.txt and write to CSV for Excel
    volatile static unsigned long int count = 0; // Declare count at the beginning of main so it's accessible everywhere
    volatile static unsigned long int gap_sum = 0;
    volatile static unsigned long int dup_count = 0;

    FILE *in = fopen("frames.txt", "r");
    if (!in) {
        perror("Error opening frames.txt");
    } else {
        FILE *out = fopen("frames.csv", "w");
        if (!out) {
            perror("Error creating frames.csv");
            fclose(in);
        } else {
            char line[1024];
            while (fgets(line, sizeof(line), in)) {
                // Ignore leading spaces
                char *start = line;
                while (*start == ' ') start++;
                if (strncmp(start, "BUFFER_IDX", 10) == 0) {
                    // Replace all spaces with commas, ignore original commas
                    char outbuf[1024];
                    int j = 0;
                    for (int i = 0; start[i] && j < (int)sizeof(outbuf) - 1; ++i) {
                        if (start[i] == ' ')
                            continue; // skip all spaces
                        if (start[i] == '\n')
                            break;
                        outbuf[j++] = start[i];
                    }
                    outbuf[j] = '\0';
                    fprintf(out, "%s\n", outbuf);
                }
            }
            fclose(out);
            fclose(in);
            printf("Frames data processessed\n");
        }
    }

    // Print lines starting with "Stream start", "First frame", or "Elapsed_Time"
    FILE *in2 = fopen("frames.txt", "r");
    if (!in2) {
        perror("Error opening frames.txt for special lines");
    } else {
        char line[1024];
        while (fgets(line, sizeof(line), in2)) {
            // Ignore leading spaces
            char *start = line;
            while (*start == ' ') start++;
            if (strncmp(start, "realsense", 9) == 0) {
                printf("%s", line);
            }
        }
        fclose(in2);
    }
    // Calculate sum of gaps in column 3 where values are not incremented by 1
    FILE *csv_gap = fopen("frames.csv", "r");
    if (!csv_gap) {
        perror("Error opening frames.csv for gap calculation");
    } else {
        char line[1024];
        unsigned long int prev_val = 0;
        int first = 1;
        while (fgets(line, sizeof(line), csv_gap)) {
            char *token;
            unsigned int col = 0;
            char tmp[1024];
            strcpy(tmp, line);
            token = strtok(tmp, ",");
            unsigned long int curr_val = 0;
            while (token && col < 3) {
                if (col == 2) {
                    curr_val = atoi(token);
                }
                token = strtok(NULL, ",");
                col++;
            }
            if (first) {
                prev_val = curr_val;
                first = 0;
                continue;
            }
            if(curr_val != prev_val)
            {
                if (curr_val != (prev_val + 1)) {
                    gap_sum += (curr_val - prev_val - 1);
                }
            }
            prev_val = curr_val;
        }
        fclose(csv_gap);
        printf("Number of Sequential frame drops: %lu\n", gap_sum);
    }

        // Highlight duplicates in column 3 and count only from the second occurrence
        FILE *csv = fopen("frames.csv", "r");
        if (!csv) {
            perror("Error opening frames.csv for update");
        } else {
            // First, count the number of lines in frames.csv
            unsigned long csv_lines_count = 0;
            char line[1024];
            while (fgets(line, sizeof(line), csv)) {
                csv_lines_count++;
            }
            rewind(csv);

            // Dynamically allocate arrays based on csv_lines_count
            char **col3 = malloc(csv_lines_count * sizeof(char *));
            char **lines = malloc(csv_lines_count * sizeof(char *));
            int *dup = calloc(csv_lines_count, sizeof(int));
            if (!col3 || !lines || !dup) {
                perror("Memory allocation failed");
                fclose(csv);
                free(col3);
                free(lines);
                free(dup);
            } else {
                count = 0;
                while (fgets(line, sizeof(line), csv)) {
                    lines[count] = strdup(line);
                    char tmp[1024];
                    strcpy(tmp, line);
                    char *token = strtok(tmp, ",");
                    int col = 0;
                    while (token && col < 3) {
                        if (col == 2) {
                            col3[count] = strdup(token);
                        }
                        token = strtok(NULL, ",");
                        col++;
                    }
                    count++;
                }
                // Find duplicates in col3, only mark from the second occurrence
                for (unsigned long i = 0; i < count; ++i) {
                    int found = 0;
                    for (unsigned long j = 0; j < i; ++j) {
                        if (col3[i] && col3[j] && strcmp(col3[i], col3[j]) == 0 && col3[i][0] != '\0') {
                            found = 1;
                            break;
                        }
                    }
                    if (found) {
                        dup[i] = 1;
                        dup_count++;
                    }
                }

                printf("Number of Duplicate Frames : %lu\n", dup_count);
                printf("Total Number of Frames drop: %lu\n", gap_sum);

                if((dup_count + gap_sum) < FRMDROPACCPTTHRESHOLD) {
                    printf("Test Passed Successfully\n");
                } else {
                    printf("Test Failed, as number of frame drops excedded the acceptable\n");
                }
                // Free allocated memory
                for (unsigned long i = 0; i < count; ++i) {
                    free(col3[i]);
                    free(lines[i]);
                }
                
                free(col3);
                free(lines);
                free(dup);
            }
            fclose(csv);
        }
}
