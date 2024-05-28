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
#define MAX_OPEN_FILES 10 

//bootsector struct
typedef struct {
    unsigned short bytesPerSector;
    unsigned char sectorsPerCluster;
    unsigned int rootCluster;
    unsigned int totalClusters; 
    unsigned int sectorsPerFAT;
    unsigned long long sizeOfImage; 
} BootSectorInfo;

unsigned int getNextCluster(int fd, unsigned int currentCluster, BootSectorInfo* bsi);

//struct that contains the current cluster, the name  and the name of the image
typedef struct {
    unsigned int currentCluster; 
    char path[512]; 
    char imageName[256]; 
} DirectoryContext;

DirectoryContext currentDirectory;

//struct to determine the number of entries a sector can hold
typedef struct {
    char name[11];
    uint8_t attr;
    uint8_t reserved[20];
    uint16_t firstClusterHigh;
    uint16_t firstClusterLow;
    uint32_t fileSize;
} DirEntry;

//struct to handle file opening
//flags determine operation to carry out based on command input
typedef struct {
    char fileName[12];    
    int flags;            
    unsigned long offset; 
    unsigned int cluster; 
    unsigned int size;    
    bool isOpen;          
} OpenFile;

OpenFile openFiles[MAX_OPEN_FILES];  //aqrray to store open files

//read data from a cluster and load it into memory buffer
bool readCluster(int fd, unsigned int clusterNum, unsigned char* buffer, BootSectorInfo* bsi) {
    unsigned long long sector = ((clusterNum - 2) * bsi->sectorsPerCluster) + bsi->rootCluster;
    off_t offset = sector * bsi->bytesPerSector;

    //seek to the cluster
    if (lseek(fd, offset, SEEK_SET) < 0) {
        perror("Error seeking cluster");
        return false;
    }
    //read the cluster
    if (read(fd, buffer, bsi->bytesPerSector * bsi->sectorsPerCluster) < 0) {
        perror("Error reading cluster");
        return false;
    }

    return true;
}

//fucntion to handle the cd command
void changeDirectory(int fd, const char* dirName, DirectoryContext* context, BootSectorInfo* bsi) {
    if (strcmp(dirName, ".") == 0) {
        printf("Staying in the current directory.\n");
        return; 
    }

    if (strcmp(dirName, "..") == 0) {
        if (strcmp(context->path, "/") == 0) {
            printf("Already in the root directory.\n");
            return; 
        }

        //check for and handle slash in input
        char* lastSlash = strrchr(context->path, '/');
        if (lastSlash != NULL) {
            if (lastSlash == context->path) {
                *(lastSlash + 1) = '\0'; 
            } else {
                *lastSlash = '\0';
            }
        }

        context->currentCluster = bsi->rootCluster;
        printf("Changed directory to parent: %s\n", context->path);
        return;
    }

    //allocate memory for reeading cluster
    unsigned char* buffer = malloc(bsi->bytesPerSector * bsi->sectorsPerCluster);
    if (!buffer) {
        printf("Failed to allocate memory\n");
        return;
    }

    //if you cannot read from the cluster, free the buffer and exit the fucntion
    if (!readCluster(fd, context->currentCluster, buffer, bsi)) {
        free(buffer);
        return;
    }

    //initialize entry pointer
    DirEntry* entry = (DirEntry*)buffer;
    int entriesCount = (bsi->bytesPerSector * bsi->sectorsPerCluster) / sizeof(DirEntry);
    bool found = false;

    //loop through direectory entries
    for (int i = 0; i < entriesCount; i++, entry++) {
        //error handling
        if (entry->name[0] == 0x00) {
            printf("Reached end of directory entries.\n");
            break; 
        }
        if ((unsigned char)entry->name[0] == 0xE5) {
            printf("Skipped a deleted entry.\n");
            continue; 
        }

        //format the name of the directory
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

            //update the path and the current cluster
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

    //if directory is not found, print error message 
    if (!found) {
        printf("Directory not found: %s\n", dirName);
    }

    free(buffer);
}

//info function
void printBootSectorInfo(const char *imagePath) {
    //open the image with correct permissions
    int fd = open(imagePath, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open image file");
        return;
    }

    //read the boot sector and print an error message if failed
    unsigned char bootSector[512];
    if (read(fd, bootSector, sizeof(bootSector)) != sizeof(bootSector)) {
        perror("Failed to read boot sector");
        close(fd);
        return;
    }

    //print the size of the image file
    struct stat st;
    if (fstat(fd, &st) != 0) {
        perror("Failed to get image file size");
        close(fd);
        return;
    }

    //populate all values for an instance of BootSectorInfo
    BootSectorInfo info = {
        .bytesPerSector = *(unsigned short *)(bootSector + 11),
        .sectorsPerCluster = *(bootSector + 13),
        .rootCluster = *(unsigned int *)(bootSector + 44),
        .sectorsPerFAT = *(unsigned int *)(bootSector + 36),
        .sizeOfImage = st.st_size
    };
    info.totalClusters = (st.st_size / (info.sectorsPerCluster * info.bytesPerSector));

    //print all data values
    printf("Bytes Per Sector: %u\n", info.bytesPerSector);
    printf("Sectors Per Cluster: %u\n", info.sectorsPerCluster);
    printf("Root Cluster: %u\n", info.rootCluster);
    printf("Total # of Clusters in Data Region: %u\n", info.totalClusters);
    printf("# of Entries in One FAT: %u\n", info.sectorsPerFAT);
    printf("Size of Image (in bytes): %llu\n", info.sizeOfImage);

    close(fd);
}

//fucntion to handle ls command
void listDirectory(int fd, DirectoryContext* context, BootSectorInfo* bsi) {
    //like all functions, allocate memeory to read from the cluster
    unsigned char* buffer = malloc(bsi->bytesPerSector * bsi->sectorsPerCluster);
    if (!buffer) {
        printf("Failed to allocate memory for reading cluster\n");
        return;
    }
    //if failed, clear the buffer and exit
    if (!readCluster(fd, context->currentCluster, buffer, bsi)) {
        free(buffer);
        return;
    }

    //initialize entry buffer and print '.' and '..'
    DirEntry* entry = (DirEntry*) buffer;
    int entriesCount = (bsi->bytesPerSector * bsi->sectorsPerCluster) / DIR_ENTRY_SIZE;
    printf(".\n..\n"); 

    //print all entries unless it was deleted
    for (int i = 0; i < entriesCount; i++, entry++) {
        if (entry->name[0] == 0x00) break; 
        if ((unsigned char)entry->name[0] == 0xE5) continue;

        printf("%.11s\n", entry->name); 
    }
    free(buffer);
}

//function to handle mkdir 
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

    //search for a free entry
    for (int i = 0; i < numEntries; i++) {
        if (entries[i].name[0] == 0x00 || (unsigned char)entries[i].name[0] == 0xE5) { 
            memset(&entries[i], 0, sizeof(DirEntry)); 
            strncpy(entries[i].name, dirName, 11); 
            entries[i].attr = ATTR_DIRECTORY;

            // Assign a new cluster for the directory different from the current one
            entries[i].firstClusterLow = context->currentCluster + 1; 
            entries[i].firstClusterHigh = 0;
            entries[i].fileSize = 0; 

            foundSpace = true;
            break;
        }
    }

    //if the directory is full, print error. if it is created successfully, print a success message
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

//function to handle the creation of the file
void createFile(int fd, const char* fileName, DirectoryContext* context, BootSectorInfo* bsi) {
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
    bool exists = false;

    //search for a free entry, similar to mkdir
    for (int i = 0; i < numEntries; i++) {
        if (entries[i].name[0] == 0x00 || (unsigned char)entries[i].name[0] == 0xE5) {
            if (!foundSpace) {
                foundSpace = true;
                memset(&entries[i], 0, sizeof(DirEntry)); 
                strncpy(entries[i].name, fileName, 11); 
                entries[i].attr = 0x00; //file attribute
                entries[i].firstClusterLow = 0; 
                entries[i].firstClusterHigh = 0;
                entries[i].fileSize = 0; 
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

    //Calculate the offset where this directory's data begins in the disk image
    if (foundSpace && !exists) {
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

//function to handle rm
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

    //search for entry to delete
    for (int i = 0; i < numEntries; i++) {
        if (entries[i].name[0] == 0x00) {
            break; 
        }

        if ((unsigned char)entries[i].name[0] == 0xE5) {
            continue; 
        }

        char formattedName[12];
        strncpy(formattedName, entries[i].name, 11);
        formattedName[11] = '\0';

        //remove trailing spaces from filename
        for (int j = 10; j >= 0; j--) {
            if (formattedName[j] == ' ') formattedName[j] = '\0';
            else break;
        }

        //mark the deleted entry as deleted
        if (strcmp(formattedName, fileName) == 0) {
            entries[i].name[0] = 0xE5; 
            fileFound = true;
            break;
        }
    }

    //if the file is found, calculate offset and print message
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

//function to handle rmdir
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

    //search for directory with formatted name, ignore deleted directories
    for (int i = 0; i < numEntries; i++) {
        if (entries[i].name[0] == 0x00) {
            break; 
        }
        if ((unsigned char)entries[i].name[0] == 0xE5) {
            continue; 
        }
        char formattedName[12];
        strncpy(formattedName, entries[i].name, 11);
        formattedName[11] = '\0';

        //remove trailing spaces from filename
        for (int j = 10; j >= 0; j--) {
            if (formattedName[j] == ' ') formattedName[j] = '\0';
            else break;
        }

        if (strcmp(formattedName, dirName) == 0 && (entries[i].attr & ATTR_DIRECTORY)) {
            found = true;

            //check if the directory is empty by attempting to read its cluster
            unsigned int dirCluster = (entries[i].firstClusterHigh << 16) | entries[i].firstClusterLow;
            unsigned char* dirBuffer = malloc(bsi->bytesPerSector * bsi->sectorsPerCluster);
            if (!dirBuffer || !readCluster(fd, dirCluster, dirBuffer, bsi)) {
                isEmpty = false; 
            } else {
                DirEntry* dirEntries = (DirEntry*)dirBuffer;
                for (int j = 0; j < numEntries; j++) {
                    if (dirEntries[j].name[0] == 0x00) {
                        break; 
                    }
                    if ((unsigned char)dirEntries[j].name[0] == 0xE5) {
                        continue; 
                    }
                    if (j > 1) { 
                        isEmpty = false;
                        break;
                    }
                }
            }
            free(dirBuffer);
            //mark the directory as deleted
            if (isEmpty) {
                entries[i].name[0] = 0xE5; 
            }
            break;
        }
    }

    if (!found) {
        printf("Error: Directory not found.\n");
    } else if (!isEmpty) {
        printf("Error: Directory is not empty or could not be read.\n");
    } else {
        //write back the updated buffer to the current directory's cluster
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

//function to handle opening a file
void openFile(int fd, const char* fileName, const char* mode, DirectoryContext* context, BootSectorInfo* bsi) {
    //check if file is already open
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (openFiles[i].isOpen && strcmp(openFiles[i].fileName, fileName) == 0) {
            printf("Error: File is already opened.\n");
            return;
        }
    }

    //find an empty slot
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

    //set flags based on mode
    int flags = -1;
    if (strcmp(mode, "-r") == 0) flags = 0;
    else if (strcmp(mode, "-w") == 0) flags = 1;
    else if (strcmp(mode, "-rw") == 0 || strcmp(mode, "-wr") == 0) flags = 2;

    if (flags == -1) {
        printf("Error: Invalid mode.\n");
        return;
    }

    //find the file in the directory
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

    //format the name and search for the correct entry
    for (int i = 0; i < entriesCount; i++, entry++) {
        if (entry->name[0] == 0x00) break; 
        if ((unsigned char)entry->name[0] == 0xE5) continue; 

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

    //print message indicating success or failure
    if (!found) {
        printf("Error: File not found.\n");
    } else {
        printf("File opened successfully: %s\n", fileName);
    }

    free(buffer);
}

//function to handles closing of a file
void closeFile(const char* fileName) {
    bool fileFound = false;
    //search for files in the list of open files and if found, close it
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (openFiles[i].isOpen && strcmp(openFiles[i].fileName, fileName) == 0) {
            openFiles[i].isOpen = false; 
            printf("File closed successfully: %s\n", fileName);
            fileFound = true;
            break;
        }
    }

    if (!fileFound) {
        printf("Error: File not found or not opened.\n");
    }
}

//function to list all open files
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

//seek the offset of the file
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

//function to handle reading of a file
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

            //calculate starting sector and offset within the sector
            unsigned int cluster = openFiles[i].cluster;
            unsigned int sectorOffset = (openFiles[i].offset / bsi->bytesPerSector) % bsi->sectorsPerCluster;
            unsigned int byteOffset = openFiles[i].offset % bsi->bytesPerSector;
            unsigned int bytesRead = 0;

            //seek file and attempt tor read - calculate te number of bytes needed to read
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

                //reset byte offset for the next secto
                bytesRead += bytesToRead;
                byteOffset = 0;
                sectorOffset++;
                if (sectorOffset >= bsi->sectorsPerCluster) {
                    sectorOffset = 0;
                    cluster = getNextCluster(fd, cluster, bsi);
                }
            }

            printf("%.*s", bytesRead, buffer); 
            free(buffer);

            //update offset
            openFiles[i].offset += bytesRead;
            printf("\nRead %u bytes from file: %s\n", bytesRead, fileName);
            break;
        }
    }

    if (!fileFound) {
        printf("Error: File not found or not opened.\n");
    }
}

//fucntion to handle finidng of the next cluster
unsigned int getNextCluster(int fd, unsigned int currentCluster, BootSectorInfo* bsi) {
    if (currentCluster < 2) {
        fprintf(stderr, "Invalid cluster number: %u\n", currentCluster);
        return 0xFFFFFFFF; //error
    }

    //FAT32 cluster entry is 4 bytes
    unsigned int fatOffset = currentCluster * 4;
    unsigned int fatSector = bsi->rootCluster + (fatOffset / bsi->bytesPerSector);
    unsigned int entOffset = fatOffset % bsi->bytesPerSector;

    //buffer to read the entry
    unsigned char buffer[4]; 

    //calculate the position to seek
    off_t position = fatSector * bsi->bytesPerSector + entOffset;

    if (lseek(fd, position, SEEK_SET) < 0) {
        perror("Error seeking in FAT");
        return 0xFFFFFFFF;  
    }

    //read the next cluster value
    if (read(fd, buffer, 4) != 4) {
        perror("Error reading FAT entry");
        return 0xFFFFFFFF;
    }

    //calculate next cluster from the entry
    unsigned int nextCluster = *(unsigned int*)buffer & 0x0FFFFFFF; 

    //end of cluster chain markers
    if (nextCluster >= 0x0FFFFFF8) {
        return 0xFFFFFFFF; 
    }
    return nextCluster;
}

//fucntion to handle writing to file NOT WORKING we tried :(
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

            //check if the offset exceeds the file size and adjust file size 
            if (newOffset > openFiles[i].size) {
                openFiles[i].size = newOffset;  
            }

            //writing data to file starting at the current offset
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

                //reset byte offset for the next sector
                bytesWritten += bytesToWrite;
                byteOffset = 0; 
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

            openFiles[i].offset = newOffset;
            printf("Data written successfully to file: %s\n", fileName);
            break;
        }
    }

    if (i == MAX_OPEN_FILES) {
        printf("Error: File not found or not opened.\n");
    }
}

//main
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

    //read the boot sector to initialize the BootSectorInfo
    unsigned char bootSector[512];
    if (read(fd, bootSector, sizeof(bootSector)) != sizeof(bootSector)) {
        perror("Failed to read boot sector");
        close(fd);
        return 1;
    }

    //initialize the boot sector info
    BootSectorInfo bsi = {
        .bytesPerSector = *(unsigned short *)(bootSector + 11),
        .sectorsPerCluster = *(bootSector + 13),
        .rootCluster = *(unsigned int *)(bootSector + 44),
        .sectorsPerFAT = *(unsigned int *)(bootSector + 36),
        .sizeOfImage = lseek(fd, 0, SEEK_END) 
    };
    bsi.totalClusters = (bsi.sizeOfImage / (bsi.sectorsPerCluster * bsi.bytesPerSector));

    //reset the file descriptor position for further operations
    lseek(fd, 0, SEEK_SET); 

    //initialize the directory context
    DirectoryContext context = {2, "/", ""}; 
    strncpy(context.imageName, argv[1], sizeof(context.imageName) - 1); 
    context.imageName[sizeof(context.imageName) - 1] = '\0'; 

    char command[256];
    //initialize infinite loop of the prompt
    while (1) {
        printf("[%s%s]/> ", context.imageName, context.path); 
        if (!fgets(command, sizeof(command), stdin)) {
            break; 
        }
        command[strcspn(command, "\n")] = 0; 

        //if statement to handle the different required commands for the system
        if (strcmp(command, "exit") == 0) {
            break;
        } else if (strcmp(command, "info") == 0) {
            printBootSectorInfo(argv[1]);
        } else if (strncmp(command, "cd ", 3) == 0) {
            char dirName[256];
            sscanf(command + 3, "%s", dirName); 
            changeDirectory(fd, dirName, &context, &bsi);
        } else if (strcmp(command, "ls") == 0) {
            listDirectory(fd, &context, &bsi);
        } else if (strncmp(command, "mkdir ", 6) == 0) {
            char dirName[256];
            sscanf(command + 6, "%255s", dirName);
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
    	    char data[1024]; 
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
