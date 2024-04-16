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
	unsigned int currentCluster; //Current directory
	char path[512]; //Full path of cd
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
        return; // No change needed if it's the current directory
    }

    if (strcmp(dirName, "..") == 0) {
        // Logic to move to the parent directory would go here
        // This is simplified; actual implementation would need to track and parse paths
        return;
    }

    // Placeholder for reading and changing directory based on 'dirName'
    // You would typically read the current directory's cluster here
    printf("Attempt to change to directory: %s\n", dirName);
    // You would also update context->currentCluster and context->path appropriately here
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

    for (int i = 0; i < entriesCount; i++, entry++) {
        if (entry->name[0] == 0x00) { // End of the directory entries
            break;
        }
        if (entry->name[0] == 0xE5) { // Entry is free (deleted file)
            continue;
        }

        // Print the entry name if it's not deleted
        printf("%.11s\n", entry->name);
    }

    free(buffer);
}


int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: ./filesys [FAT32 ISO]\n");
        return 1;
    }

	int fd = open(argv[1], O_RDONLY);
    	if (fd == -1) {
        	perror("Error opening file");
        	return 1;
    	}

// Initialize boot sector info and directory context
    BootSectorInfo bsi; // This needs to be properly initialized, perhaps with another function
    DirectoryContext context = {0, ""};
    context.currentCluster = 2; // Typically the root directory, but should be set based on actual data
    strcpy(context.path, "/"); // Root path initialization

    char command[256];
    while (1) {
        printf("[%s]/> ", context.path);
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
            //changeDirectory(fd, dirName, &context, &bsi);
        } else if (strcmp(command, "ls") == 0) {
            listDirectory(fd, &context, &bsi);
        } else {
            printf("Unknown command\n");
        }
    }

    close(fd);

    return 0;
}
