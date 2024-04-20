#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>

#define DIR_ENTRY_SIZE 32
#define ATTR_DIRECTORY 0x10

typedef struct {
    unsigned short bytesPerSector;
    unsigned char sectorsPerCluster;
    unsigned int rootCluster;
    unsigned int totalClusters; // Calculated based on the image size and sectors per cluster
    unsigned int sectorsPerFAT;
    unsigned long long sizeOfImage; // Calculated from file size
} BootSectorInfo;

typedef struct {
    unsigned int currentCluster; // Current directory cluster
    char path[512]; // Full path of the current directory
    char imageName[256]; // Name of the image file
} DirectoryContext;

DirectoryContext currentDirectory;

typedef struct {
    char name[11];
    uint8_t attr;
    uint8_t reserved[20];
    uint16_t firstClusterHigh;
    uint16_t firstClusterLow;
    uint32_t fileSize;
} DirEntry;

bool readCluster(int fd, unsigned int clusterNum, unsigned char* buffer, BootSectorInfo* bsi) {
    // Convert cluster number to sector number
    unsigned long long sector = ((clusterNum - 2) * bsi->sectorsPerCluster) + bsi->rootCluster;
    off_t offset = sector * bsi->bytesPerSector;

    if (lseek(fd, offset, SEEK_SET) < 0) {
        perror("Error seeking cluster");
        return false;
    }

    if (read(fd, buffer, bsi->bytesPerSector * bsi->sectorsPerCluster) < 0) {
        perror("Error reading cluster");
        return false;
    }

    return true;
}

void changeDirectory(int fd, const char* dirName, DirectoryContext* context, BootSectorInfo* bsi) {
    if (strcmp(dirName, ".") == 0) {
        printf("Staying in the current directory.\n");
        return; // Stay in the current directory
    }

    if (strcmp(dirName, "..") == 0) {
        // Handle moving to the parent directory
        if (strcmp(context->path, "/") == 0) {
            printf("Already at root directory.\n");
            return; // Cannot go above root directory
        }

        // Remove the last directory from the path
        char* lastSlash = strrchr(context->path, '/');
        if (lastSlash != NULL) {
            if (lastSlash == context->path) {
                // If the last slash is the beginning of the path, then we're moving back to root
                *(lastSlash + 1) = '\0'; // Keep the root slash only
            } else {
                *lastSlash = '\0'; // Cut the path at the last slash
            }
        }

        // Assuming parent directory is root for now; should dynamically find the actual parent cluster
        context->currentCluster = bsi->rootCluster;
        printf("Changed directory to parent: %s\n", context->path);
        return;
    }

    // Allocate buffer for reading directory entries
    unsigned char* buffer = malloc(bsi->bytesPerSector * bsi->sectorsPerCluster);
    if (!buffer) {
        printf("Failed to allocate memory for reading cluster\n");
        return;
    }

    if (!readCluster(fd, context->currentCluster, buffer, bsi)) {
        free(buffer);
        return;
    }

    DirEntry* entry = (DirEntry*)buffer;
    int entriesCount = (bsi->bytesPerSector * bsi->sectorsPerCluster) / sizeof(DirEntry);
    bool found = false;

    for (int i = 0; i < entriesCount; i++, entry++) {
        if (entry->name[0] == 0x00) {
            printf("Reached end of directory entries.\n");
            break; // End of directory entries
        }
        if ((unsigned char)entry->name[0] == 0xE5) {
            printf("Skipped a deleted entry.\n");
            continue; // Skip deleted entry
        }

        char formattedName[12];
        strncpy(formattedName, entry->name, 11);
        formattedName[11] = '\0';

        for (int j = 10; j >= 0; j--) {
            if (formattedName[j] == ' ') formattedName[j] = '\0';
            else break;
        }

        if ((entry->attr & ATTR_DIRECTORY) && strcmp(formattedName, dirName) == 0) {
            unsigned int newCluster = (entry->firstClusterHigh << 16) | entry->firstClusterLow;
            if (newCluster == 0) newCluster = bsi->rootCluster; 

            char newPath[512];
            if (snprintf(newPath, sizeof(newPath), "%s/%s", context->path, dirName) >= (int)sizeof(newPath)) {
                printf("Error: New path too long\n");
                free(buffer);
                return;
            }
            strncpy(context->path, newPath, sizeof(context->path));
            context->path[sizeof(context->path) - 1] = '\0'; 

            context->currentCluster = newCluster;
            found = true;
            printf("Changed directory to %s\n", dirName);
            break;
        }
    }

    if (!found) {
        printf("Directory not found: %s\n", dirName);
    }

    free(buffer);
}

void printBootSectorInfo(const char *imagePath) {
    int fd = open(imagePath, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open image file");
        return;
    }

    unsigned char bootSector[512];
    if (read(fd, bootSector, sizeof(bootSector)) != sizeof(bootSector)) {
        perror("Failed to read boot sector");
        close(fd);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        perror("Failed to get image file size");
        close(fd);
        return;
    }

    BootSectorInfo info = {
        .bytesPerSector = *(unsigned short *)(bootSector + 11),
        .sectorsPerCluster = *(bootSector + 13),
        .rootCluster = *(unsigned int *)(bootSector + 44),
        .sectorsPerFAT = *(unsigned int *)(bootSector + 36),
        .sizeOfImage = st.st_size
    };
    info.totalClusters = (st.st_size / (info.sectorsPerCluster * info.bytesPerSector));

    printf("Bytes Per Sector: %u\n", info.bytesPerSector);
    printf("Sectors Per Cluster: %u\n", info.sectorsPerCluster);
    printf("Root Cluster: %u\n", info.rootCluster);
    printf("Total # of Clusters in Data Region: %u\n", info.totalClusters);
    printf("# of Entries in One FAT: %u\n", info.sectorsPerFAT);
    printf("Size of Image (in bytes): %llu\n", info.sizeOfImage);

    close(fd);
}

void listDirectory(int fd, DirectoryContext* context, BootSectorInfo* bsi) {
    unsigned char* buffer = malloc(bsi->bytesPerSector * bsi->sectorsPerCluster);
    if (!buffer) {
        printf("Failed to allocate memory for reading cluster\n");
        return;
    }

    if (!readCluster(fd, context->currentCluster, buffer, bsi)) {
        free(buffer);
        return;
    }

    DirEntry* entry = (DirEntry*) buffer;
    int entriesCount = (bsi->bytesPerSector * bsi->sectorsPerCluster) / DIR_ENTRY_SIZE;
    printf(".\n..\n"); // Always list '.' and '..'

    for (int i = 0; i < entriesCount; i++, entry++) {
        if (entry->name[0] == 0x00) break; // End of the directory entries
        if ((unsigned char)entry->name[0] == 0xE5) continue; // Skip deleted entries

        printf("%.11s\n", entry->name); // Print the entry name if it's not deleted
    }

    free(buffer);
}

void createDirectory(int fd, const char* dirName, DirectoryContext* context, BootSectorInfo* bsi) {
    unsigned char* buffer = malloc(bsi->bytesPerSector * bsi->sectorsPerCluster);
    if (!buffer) {
        printf("Failed to allocate memory for directory cluster\n");
        return;
    }

    if (!readCluster(fd, context->currentCluster, buffer, bsi)) {
        free(buffer);
        return;
    }

    DirEntry* entries = (DirEntry*) buffer;
    int numEntries = (bsi->bytesPerSector * bsi->sectorsPerCluster) / sizeof(DirEntry);
    bool foundSpace = false;

    for (int i = 0; i < numEntries; i++) {
        if (entries[i].name[0] == 0x00 || (unsigned char)entries[i].name[0] == 0xE5) { // Free entry found
            memset(&entries[i], 0, sizeof(DirEntry)); // Clear the entry to prepare it for new directory
            strncpy(entries[i].name, dirName, 11); // Format name to FAT32 8.3 standard
            entries[i].attr = ATTR_DIRECTORY;

            // Assign a new cluster for the directory, different from the current one
            // If your simulation tracks free clusters, assign one from the free list
            entries[i].firstClusterLow = context->currentCluster + 1; // Simplified example; ensure this doesn't overlap real data
            entries[i].firstClusterHigh = 0;
            entries[i].fileSize = 0; // Directory size is always zero

            foundSpace = true;
            break;
        }
    }

    if (!foundSpace) {
        printf("No space in current directory to create new directory\n");
    } else {
        off_t offset = (context->currentCluster - 2) * bsi->sectorsPerCluster * bsi->bytesPerSector + bsi->rootCluster * bsi->bytesPerSector;
        if (lseek(fd, offset, SEEK_SET) < 0) {
            perror("Error seeking to write new directory entry");
        } else if (write(fd, buffer, bsi->bytesPerSector * bsi->sectorsPerCluster) < 0) {
            perror("Error writing new directory entry");
        } else {
            printf("Directory created successfully\n");
        }
    }

    free(buffer);
}

void createFile(int fd, const char* fileName, DirectoryContext* context, BootSectorInfo* bsi) {
    unsigned char* buffer = malloc(bsi->bytesPerSector * bsi->sectorsPerCluster);
    if (!buffer) {
        printf("Failed to allocate memory for directory cluster\n");
        return;
    }

    // Read the cluster where the current directory resides
    if (!readCluster(fd, context->currentCluster, buffer, bsi)) {
        free(buffer);
        return;
    }

    DirEntry* entries = (DirEntry*) buffer;
    int numEntries = (bsi->bytesPerSector * bsi->sectorsPerCluster) / sizeof(DirEntry);
    bool foundSpace = false;
    bool exists = false;

    for (int i = 0; i < numEntries; i++) {
        if (entries[i].name[0] == 0x00 || (unsigned char)entries[i].name[0] == 0xE5) {
            if (!foundSpace) {
                foundSpace = true;
                memset(&entries[i], 0, sizeof(DirEntry)); // Clear the entry to prepare it for new file
                strncpy(entries[i].name, fileName, 11); // Format name to FAT32 8.3 standard
                entries[i].attr = 0x00; // Normal file attribute
                entries[i].firstClusterLow = 0; // No cluster allocated for 0 byte file
                entries[i].firstClusterHigh = 0;
                entries[i].fileSize = 0; // File size is zero bytes
            }
        } else {
            char formattedName[12];
            strncpy(formattedName, entries[i].name, 11);
            formattedName[11] = '\0';

            for (int j = 10; j >= 0; j--) {
                if (formattedName[j] == ' ') formattedName[j] = '\0';
                else break;
            }

            if (strcmp(formattedName, fileName) == 0) {
                printf("Error: A file or directory with this name already exists.\n");
                exists = true;
                break;
            }
        }
    }

    if (foundSpace && !exists) {
        // Calculate the offset where this directory's data begins in the disk image
        unsigned long long sector = ((context->currentCluster - 2) * bsi->sectorsPerCluster) + bsi->rootCluster;
        off_t offset = sector * bsi->bytesPerSector;

        if (lseek(fd, offset, SEEK_SET) < 0) {
            perror("Error seeking to write new file entry");
        } else if (write(fd, buffer, bsi->bytesPerSector * bsi->sectorsPerCluster) < 0) {
            perror("Error writing new file entry");
        } else {
            printf("File created successfully\n");
        }
    }

    free(buffer);
}


void removeFile(int fd, const char* fileName, DirectoryContext* context, BootSectorInfo* bsi) {
    unsigned char* buffer = malloc(bsi->bytesPerSector * bsi->sectorsPerCluster);
    if (!buffer) {
        printf("Failed to allocate memory for directory cluster\n");
        return;
    }

    if (!readCluster(fd, context->currentCluster, buffer, bsi)) {
        free(buffer);
        return;
    }

    DirEntry* entries = (DirEntry*) buffer;
    int numEntries = (bsi->bytesPerSector * bsi->sectorsPerCluster) / sizeof(DirEntry);
    bool fileFound = false;

    for (int i = 0; i < numEntries; i++) {
        if (entries[i].name[0] == 0x00) {
            break; // End of directory entries
        }

        if ((unsigned char)entries[i].name[0] == 0xE5) {
            continue; // Skip deleted entries
        }

        char formattedName[12];
        strncpy(formattedName, entries[i].name, 11);
        formattedName[11] = '\0';

        // Remove trailing spaces from filename for comparison
        for (int j = 10; j >= 0; j--) {
            if (formattedName[j] == ' ') formattedName[j] = '\0';
            else break;
        }

        if (strcmp(formattedName, fileName) == 0) {
            entries[i].name[0] = 0xE5; // Mark the entry as deleted
            fileFound = true;
            break;
        }
    }

    if (fileFound) {
        off_t offset = (context->currentCluster - 2) * bsi->sectorsPerCluster * bsi->bytesPerSector + bsi->rootCluster * bsi->bytesPerSector;
        if (lseek(fd, offset, SEEK_SET) < 0) {
            perror("Error seeking to update directory entry");
        } else if (write(fd, buffer, bsi->bytesPerSector * bsi->sectorsPerCluster) < 0) {
            perror("Error writing updated directory entry");
        } else {
            printf("File removed successfully\n");
        }
    } else {
        printf("Error: File not found.\n");
    }

    free(buffer);
}

void removeDirectory(int fd, const char* dirName, DirectoryContext* context, BootSectorInfo* bsi) {
    if (strcmp(dirName, ".") == 0 || strcmp(dirName, "..") == 0) {
        printf("Error: Cannot remove '.' or '..'\n");
        return;
    }

    unsigned char* buffer = malloc(bsi->bytesPerSector * bsi->sectorsPerCluster);
    if (!buffer) {
        printf("Failed to allocate memory for directory cluster\n");
        return;
    }

    if (!readCluster(fd, context->currentCluster, buffer, bsi)) {
        free(buffer);
        return;
    }

    DirEntry* entries = (DirEntry*)buffer;
    int numEntries = (bsi->bytesPerSector * bsi->sectorsPerCluster) / sizeof(DirEntry);
    bool found = false;
    bool isEmpty = true;

    // First pass: check existence and emptiness
    for (int i = 0; i < numEntries; i++) {
        if (entries[i].name[0] == 0x00) {
            break; // End of directory entries
        }
        if ((unsigned char)entries[i].name[0] == 0xE5) {
            continue; // Skip deleted entries
        }

        char formattedName[12];
        strncpy(formattedName, entries[i].name, 11);
        formattedName[11] = '\0';

        // Remove trailing spaces from filename for comparison
        for (int j = 10; j >= 0; j--) {
            if (formattedName[j] == ' ') formattedName[j] = '\0';
            else break;
        }

        if (strcmp(formattedName, dirName) == 0 && (entries[i].attr & ATTR_DIRECTORY)) {
            found = true;

            // Check if the directory is empty by attempting to read its cluster
            unsigned int dirCluster = (entries[i].firstClusterHigh << 16) | entries[i].firstClusterLow;
            unsigned char* dirBuffer = malloc(bsi->bytesPerSector * bsi->sectorsPerCluster);
            if (!dirBuffer || !readCluster(fd, dirCluster, dirBuffer, bsi)) {
                isEmpty = false; // Assume not empty if we fail to read
            } else {
                DirEntry* dirEntries = (DirEntry*)dirBuffer;
                for (int j = 0; j < numEntries; j++) {
                    if (dirEntries[j].name[0] == 0x00) {
                        break; // End of directory entries
                    }
                    if ((unsigned char)dirEntries[j].name[0] == 0xE5) {
                        continue; // Skip deleted entries
                    }
                    if (j > 1) { // More than just '.' and '..'
                        isEmpty = false;
                        break;
                    }
                }
            }
            free(dirBuffer);

            if (isEmpty) {
                entries[i].name[0] = 0xE5; // Mark the directory as deleted
            }
            break;
        }
    }

    if (!found) {
        printf("Error: Directory not found.\n");
    } else if (!isEmpty) {
        printf("Error: Directory is not empty or could not be read.\n");
    } else {
        // Write back the updated buffer to the current directory's cluster
        off_t offset = ((context->currentCluster - 2) * bsi->sectorsPerCluster + bsi->rootCluster) * bsi->bytesPerSector;
        if (lseek(fd, offset, SEEK_SET) < 0) {
            perror("Error seeking to update directory");
        } else if (write(fd, buffer, bsi->bytesPerSector * bsi->sectorsPerCluster) < 0) {
            perror("Error writing updated directory");
        } else {
            printf("Directory removed successfully\n");
        }
    }

    free(buffer);
}


int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: ./filesys [FAT32 ISO]\n");
        return 1;
    }

    int fd = open(argv[1], O_RDWR);
    if (fd == -1) {
        perror("Error opening file");
        return 1;
    }

    // Read the boot sector to initialize the BootSectorInfo
    unsigned char bootSector[512];
    if (read(fd, bootSector, sizeof(bootSector)) != sizeof(bootSector)) {
        perror("Failed to read boot sector");
        close(fd);
        return 1;
    }

    BootSectorInfo bsi = {
        .bytesPerSector = *(unsigned short *)(bootSector + 11),
        .sectorsPerCluster = *(bootSector + 13),
        .rootCluster = *(unsigned int *)(bootSector + 44),
        .sectorsPerFAT = *(unsigned int *)(bootSector + 36),
        .sizeOfImage = lseek(fd, 0, SEEK_END) // Fetch the size of the image by seeking to the end
    };
    bsi.totalClusters = (bsi.sizeOfImage / (bsi.sectorsPerCluster * bsi.bytesPerSector));

    lseek(fd, 0, SEEK_SET); // Reset the file descriptor position for further operations

    DirectoryContext context = {2, "/", ""}; // Initialize the context
    strncpy(context.imageName, argv[1], sizeof(context.imageName) - 1); // Store the image file name
    context.imageName[sizeof(context.imageName) - 1] = '\0'; // Ensure null-termination

    char command[256];
    while (1) {
        printf("[%s%s]/> ", context.imageName, context.path); // Display both image name and path
        if (!fgets(command, sizeof(command), stdin)) {
            break; // Exit on EOF
        }
        command[strcspn(command, "\n")] = 0; // Remove newline character

        if (strcmp(command, "exit") == 0) {
            break;
        } else if (strcmp(command, "info") == 0) {
            printBootSectorInfo(argv[1]);
        } else if (strncmp(command, "cd ", 3) == 0) {
            char dirName[256];
            sscanf(command + 3, "%s", dirName); // Assuming directory names don't contain spaces
            changeDirectory(fd, dirName, &context, &bsi);
        } else if (strcmp(command, "ls") == 0) {
            listDirectory(fd, &context, &bsi);
        } else if (strncmp(command, "mkdir ", 6) == 0) {
            char dirName[256];
            sscanf(command + 6, "%255s", dirName); // Extract directory name from command
            createDirectory(fd, dirName, &context, &bsi);
        } else if (strncmp(command, "creat ", 6) == 0) {
            char filename[256];
            sscanf(command + 6, "%255s", filename);
            createFile(fd, filename, &context, &bsi);
        } else if (strncmp(command, "rm ", 3) == 0) {
            char filename[256];
            sscanf(command + 3, "%255s", filename);
            removeFile(fd, filename, &context, &bsi);
        } else if (strncmp(command, "rmdir ", 6) == 0) {
            char dirName[256];
            sscanf(command + 6, "%255s", dirName);
            removeDirectory(fd, dirName, &context, &bsi);
        } else {
            printf("Unknown command\n");
        }
    }

    close(fd);
    return 0;
}
