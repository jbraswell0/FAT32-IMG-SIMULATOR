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
#define MAX_OPEN_FILES 10  // Maximum number of open files


typedef struct {
    unsigned short bytesPerSector;
    unsigned char sectorsPerCluster;
    unsigned int rootCluster;
    unsigned int totalClusters; // Calculated based on the image size and sectors per cluster
    unsigned int sectorsPerFAT;
    unsigned long long sizeOfImage; // Calculated from file size
} BootSectorInfo;

unsigned int getNextCluster(int fd, unsigned int currentCluster, BootSectorInfo* bsi);

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

typedef struct {
    char fileName[12];    // File name (considering 8.3 format)
    int flags;            // 0 for read, 1 for write, 2 for read/write
    unsigned long offset; // Offset within the file
    unsigned int cluster; // Starting cluster of the file
    unsigned int size;    // Size of the file in bytes
    bool isOpen;          // Is file currently open
} OpenFile;

OpenFile openFiles[MAX_OPEN_FILES];  // Array to store open files


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

void initializeOpenFiles() {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        openFiles[i].isOpen = false;
    }
}

void openFile(int fd, const char* fileName, const char* mode, DirectoryContext* context, BootSectorInfo* bsi) {
    // Check if file is already open
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (openFiles[i].isOpen && strcmp(openFiles[i].fileName, fileName) == 0) {
            printf("Error: File is already opened.\n");
            return;
        }
    }

    // Find an empty slot
    int index = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!openFiles[i].isOpen) {
            index = i;
            break;
        }
    }

    if (index == -1) {
        printf("Error: Too many open files.\n");
        return;
    }

    // Set flags based on mode
    int flags = -1;
    if (strcmp(mode, "-r") == 0) flags = 0;
    else if (strcmp(mode, "-w") == 0) flags = 1;
    else if (strcmp(mode, "-rw") == 0 || strcmp(mode, "-wr") == 0) flags = 2;

    if (flags == -1) {
        printf("Error: Invalid mode.\n");
        return;
    }

    // Find the file in the directory
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
        if (entry->name[0] == 0x00) break; // End of the directory entries
        if ((unsigned char)entry->name[0] == 0xE5) continue; // Skip deleted entries

        char formattedName[12];
        strncpy(formattedName, entry->name, 11);
        formattedName[11] = '\0';

        for (int j = 10; j >= 0; j--) {
            if (formattedName[j] == ' ') formattedName[j] = '\0';
            else break;
        }

        if (strcmp(formattedName, fileName) == 0 && !(entry->attr & ATTR_DIRECTORY)) {
            openFiles[index].isOpen = true;
            strncpy(openFiles[index].fileName, fileName, 11);
            openFiles[index].flags = flags;
            openFiles[index].offset = 0;
            openFiles[index].cluster = (entry->firstClusterHigh << 16) | entry->firstClusterLow;
            openFiles[index].size = entry->fileSize;
            found = true;
            break;
        }
    }

    if (!found) {
        printf("Error: File not found.\n");
    } else {
        printf("File opened successfully: %s\n", fileName);
    }

    free(buffer);
}

void closeFile(const char* fileName) {
    bool fileFound = false;

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (openFiles[i].isOpen && strcmp(openFiles[i].fileName, fileName) == 0) {
            openFiles[i].isOpen = false; // Mark the file as closed
            printf("File closed successfully: %s\n", fileName);
            fileFound = true;
            break;
        }
    }

    if (!fileFound) {
        printf("Error: File not found or not opened.\n");
    }
}

void listOpenFiles(DirectoryContext* context) {
    bool anyFileOpen = false;
    printf("Opened Files:\n");
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (openFiles[i].isOpen) {
            anyFileOpen = true;
            const char* modeString;
            switch (openFiles[i].flags) {
                case 0: modeString = "Read-Only"; break;
                case 1: modeString = "Write-Only"; break;
                case 2: modeString = "Read-Write"; break;
                default: modeString = "Unknown"; break;
            }
            printf("Index: %d, File: %s, Mode: %s, Offset: %lu, Path: %s\n",
                   i, openFiles[i].fileName, modeString, openFiles[i].offset, context->path);
        }
    }

    if (!anyFileOpen) {
        printf("No files are currently opened.\n");
    }
}

void seekFile(const char* fileName, unsigned long newOffset) {
    bool fileFound = false;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (openFiles[i].isOpen && strcmp(openFiles[i].fileName, fileName) == 0) {
            fileFound = true;
            if (newOffset > openFiles[i].size) {
                printf("Error: Offset is larger than the size of the file.\n");
            } else {
                openFiles[i].offset = newOffset;
                printf("Offset set to %lu for file: %s\n", newOffset, fileName);
            }
            break;
        }
    }

    if (!fileFound) {
        printf("Error: File not found or not opened.\n");
    }
}

void readFile(int fd, const char* fileName, unsigned int size, BootSectorInfo* bsi) {
    bool fileFound = false;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (openFiles[i].isOpen && strcmp(openFiles[i].fileName, fileName) == 0) {
            if (openFiles[i].flags == 1) {
                printf("Error: File is not opened for reading.\n");
                return;
            }
            fileFound = true;
            unsigned char* buffer = malloc(size);
            if (!buffer) {
                printf("Memory allocation failed\n");
                return;
            }
            unsigned int readSize = size;
            if (openFiles[i].offset + size > openFiles[i].size) {
                readSize = openFiles[i].size - openFiles[i].offset;
            }

            // Calculate starting sector and offset within the sector
            unsigned int cluster = openFiles[i].cluster;
            unsigned int sectorOffset = (openFiles[i].offset / bsi->bytesPerSector) % bsi->sectorsPerCluster;
            unsigned int byteOffset = openFiles[i].offset % bsi->bytesPerSector;
            unsigned int bytesRead = 0;

            while (bytesRead < readSize) {
                unsigned long long sector = ((cluster - 2) * bsi->sectorsPerCluster) + bsi->rootCluster + sectorOffset;
                off_t sectorStart = sector * bsi->bytesPerSector;

                if (lseek(fd, sectorStart + byteOffset, SEEK_SET) < 0) {
                    perror("Error seeking file");
                    free(buffer);
                    return;
                }

                unsigned int bytesToRead = bsi->bytesPerSector - byteOffset;
                if (bytesRead + bytesToRead > readSize) {
                    bytesToRead = readSize - bytesRead;
                }

                if (read(fd, buffer + bytesRead, bytesToRead) < 0) {
                    perror("Error reading file");
                    free(buffer);
                    return;
                }

                bytesRead += bytesToRead;
                byteOffset = 0;  // Reset byte offset for the next sector
                sectorOffset++;
                if (sectorOffset >= bsi->sectorsPerCluster) {
                    sectorOffset = 0;
                    // Assuming a simple FAT chain lookup function that gets the next cluster
                    cluster = getNextCluster(fd, cluster, bsi);
                }
            }

            printf("%.*s", bytesRead, buffer);  // Print the read data
            free(buffer);

            // Update offset
            openFiles[i].offset += bytesRead;
            printf("\nRead %u bytes from file: %s\n", bytesRead, fileName);
            break;
        }
    }

    if (!fileFound) {
        printf("Error: File not found or not opened.\n");
    }
}

unsigned int getNextCluster(int fd, unsigned int currentCluster, BootSectorInfo* bsi) {
    if (currentCluster < 2) {
        fprintf(stderr, "Invalid cluster number: %u\n", currentCluster);
        return 0xFFFFFFFF;  // Indicates an error or end of cluster chain
    }

    // FAT32 cluster entry is 4 bytes
    unsigned int fatOffset = currentCluster * 4;
    unsigned int fatSector = bsi->rootCluster + (fatOffset / bsi->bytesPerSector);
    unsigned int entOffset = fatOffset % bsi->bytesPerSector;

    // Buffer to read the entry from FAT
    unsigned char buffer[4];  // FAT32 entries are 4 bytes

    // Calculate the position to seek to in the FAT
    off_t position = fatSector * bsi->bytesPerSector + entOffset;

    if (lseek(fd, position, SEEK_SET) < 0) {
        perror("Error seeking in FAT");
        return 0xFFFFFFFF;  // Indicates an error or end of cluster chain
    }

    // Read the next cluster value from the FAT
    if (read(fd, buffer, 4) != 4) {
        perror("Error reading FAT entry");
        return 0xFFFFFFFF;  // Indicates an error or end of cluster chain
    }

    // Calculate next cluster from the entry
    unsigned int nextCluster = *(unsigned int*)buffer & 0x0FFFFFFF;  // Mask to lower 28 bits

    // End of cluster chain markers for FAT32
    if (nextCluster >= 0x0FFFFFF8) {
        return 0xFFFFFFFF;  // End of chain
    }

    return nextCluster;
}

void writeFile(int fd, const char* fileName, const char* data, BootSectorInfo* bsi) {
    int i;
    for (i = 0; i < MAX_OPEN_FILES; i++) {
        if (openFiles[i].isOpen && strcmp(openFiles[i].fileName, fileName) == 0) {
            if (openFiles[i].flags == 0) {
                printf("Error: File is not opened for writing.\n");
                return;
            }

            unsigned int dataSize = strlen(data);
            unsigned int newOffset = openFiles[i].offset + dataSize;

            // Check if the offset exceeds the file size and adjust file size if needed
            if (newOffset > openFiles[i].size) {
                openFiles[i].size = newOffset;  // Update file size to accommodate new data
            }

            // Writing data to file starting at the current offset
            unsigned int cluster = openFiles[i].cluster;
            unsigned int sectorOffset = (openFiles[i].offset / bsi->bytesPerSector) % bsi->sectorsPerCluster;
            unsigned int byteOffset = openFiles[i].offset % bsi->bytesPerSector;
            unsigned int bytesWritten = 0;

            while (bytesWritten < dataSize) {
                unsigned long long sector = ((cluster - 2) * bsi->sectorsPerCluster) + bsi->rootCluster + sectorOffset;
                off_t sectorStart = sector * bsi->bytesPerSector;

                if (lseek(fd, sectorStart + byteOffset, SEEK_SET) < 0) {
                    perror("Error seeking in file for writing");
                    return;
                }

                unsigned int bytesToWrite = bsi->bytesPerSector - byteOffset;
                if (bytesWritten + bytesToWrite > dataSize) {
                    bytesToWrite = dataSize - bytesWritten;
                }

                if (write(fd, data + bytesWritten, bytesToWrite) < 0) {
                    perror("Error writing to file");
                    return;
                }

                bytesWritten += bytesToWrite;
                byteOffset = 0;  // Reset byte offset for the next sector
                sectorOffset++;
                if (sectorOffset >= bsi->sectorsPerCluster) {
                    sectorOffset = 0;
                    cluster = getNextCluster(fd, cluster, bsi);
                    if (cluster == 0xFFFFFFFF) {
                        printf("Error: Failed to find next cluster.\n");
                        return;
                    }
                }
            }

            openFiles[i].offset = newOffset;  // Update file offset after writing
            printf("Data written successfully to file: %s\n", fileName);
            break;
        }
    }

    if (i == MAX_OPEN_FILES) {
        printf("Error: File not found or not opened.\n");
    }
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
        } else if (strncmp(command, "open ", 5) == 0) {
    	    char fileName[256], mode[4];
    	    sscanf(command + 5, "%s %s", fileName, mode);
    	    openFile(fd, fileName, mode, &context, &bsi);
	} else if (strncmp(command, "close ", 6) == 0) {
            char fileName[256];
    	    sscanf(command + 6, "%255s", fileName);
    	    closeFile(fileName);
 	} else if (strcmp(command, "lsof") == 0) {
	    listOpenFiles(&context);
	} else if (strncmp(command, "lseek ", 6) == 0) {
    	    char fileName[256];
    	    unsigned long offset;
    	    if (sscanf(command + 6, "%s %lu", fileName, &offset) == 2) {
        	seekFile(fileName, offset);
    	    } else {
        	printf("Invalid command format. Usage: lseek [FILENAME] [OFFSET]\n");
    	    }
	} else if (strncmp(command, "read ", 5) == 0) {
   	    char fileName[256];
    	    unsigned int size;
    	    if (sscanf(command + 5, "%s %u", fileName, &size) == 2) {
        	readFile(fd, fileName, size, &bsi);
    	    } else {
        	printf("Invalid command format. Usage: read [FILENAME] [SIZE]\n");
    	    }
	} else if (strncmp(command, "write ", 6) == 0) {
    	    char fileName[256];
    	    char data[1024];  // Assuming a maximum write size for simplicity
    	  if (sscanf(command + 6, "%s \"%1023[^\"]\"", fileName, data) == 2) {
            writeFile(fd, fileName, data, &bsi);
    	  } else {
            printf("Invalid command format. Usage: write [FILENAME] \"[STRING]\"\n");
    	  }
	} else {
            printf("Unknown command\n");
        }
    }

    close(fd);
    return 0;
}
