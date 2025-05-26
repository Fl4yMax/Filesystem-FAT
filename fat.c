#include "fat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#define CLUSTER_SIZE 512
PartitionTable pt[4];
Fat16BootSector bs;
FILE *in;
unsigned int *fat_table = NULL;
unsigned int current_cluster = 0;
unsigned int current_dir_offset = 0;
char current_path[256] = "Groot";

void format_filename(const unsigned char *filename, const unsigned char *ext, char *formatted)
{
    char base_name[9];
    char extension[4];

    // Copy and null-terminate filename
    memcpy(base_name, filename, 8);
    base_name[8] = '\0';

    // Trim trailing spaces from filename
    int i = 7;
    while (i >= 0 && base_name[i] == ' ')
    {
        base_name[i--] = '\0';
    }

    // Copy and null-terminate extension
    memcpy(extension, ext, 3);
    extension[3] = '\0';

    // Trim trailing spaces from extension
    i = 2;
    while (i >= 0 && extension[i] == ' ')
    {
        extension[i--] = '\0';
    }

    // Format as filename.ext or just filename if no extension
    if (extension[0] != '\0')
    {
        sprintf(formatted, "%s.%s", base_name, extension);
    }
    else
    {
        strcpy(formatted, base_name);
    }
}

// Convert FAT16 time format to human-readable string
void format_time(unsigned short fat_time, unsigned short fat_date, char *time_str)
{
    unsigned int year = ((fat_date >> 9) & 0x7F) + 1980;
    unsigned int month = (fat_date >> 5) & 0x0F;
    unsigned int day = fat_date & 0x1F;

    unsigned int hour = (fat_time >> 11) & 0x1F;
    unsigned int minute = (fat_time >> 5) & 0x3F;

    sprintf(time_str, "%02u/%02u/%04u %02u:%02u", month, day, year, hour, minute);
}

// Function to display directory listing in DOS-like format
void print_directory()
{
    Fat16Entry entry;
    char formatted_name[13];
    char time_str[20];
    int file_count = 0;
    int dir_count = 0;
    unsigned long total_bytes = 0;

    // Calculate directory offset based on current position
    unsigned int dir_offset;
    if (current_cluster == 0)
    {
        dir_offset = (pt[0].start_sector + bs.reserved_sectors +
                      bs.number_of_fats * bs.fat_size_sectors) *
                     bs.sector_size;
    }
    else
    {
        dir_offset = (pt[0].start_sector + bs.reserved_sectors +
                      bs.number_of_fats * bs.fat_size_sectors +
                      bs.root_dir_entries * 32 / bs.sector_size +
                      (current_cluster - 2) * bs.sectors_per_cluster) *
                     bs.sector_size;
    }

    fseek(in, dir_offset, SEEK_SET);

    printf("\nVolume in drive: %.11s\n", bs.volume_label);
    // printf("Directory of %s\n\n", current_cluster == 0 ? "ROOT" : "ADR1");
    printf("Directory of %s\n\n", current_path);
    printf("   Date    Time        Name           Size\n");
    printf("-----------------------------------------\n");

    int entries_to_read = (current_cluster == 0) ? bs.root_dir_entries : (bs.sectors_per_cluster * bs.sector_size) / sizeof(Fat16Entry);

    for (int i = 0; i < entries_to_read; i++)
    {
        if (fread(&entry, sizeof(entry), 1, in) != 1)
            break;

        if (entry.filename[0] == 0x00)
            break;
        if (entry.filename[0] == 0xE5)
            continue;

        format_filename(entry.filename, entry.ext, formatted_name);
        format_time(entry.modify_time, entry.modify_date, time_str);

        if (entry.attributes & 0x10)
        {
            printf("%s  %-12s <DIR>\n", time_str, formatted_name);
            dir_count++;
        }
        else
        {
            printf("%s  %-12s %8u\n", time_str, formatted_name, entry.file_size);
            file_count++;
            total_bytes += entry.file_size;
        }
    }

    printf("-----------------------------------------\n");
    printf("%d File(s)    %lu bytes\n", file_count, total_bytes);
    printf("%d Dir(s)     %lu bytes free\n", dir_count,
           (unsigned long)(bs.sectors_per_cluster * bs.sector_size *
                           (bs.total_sectors_int - bs.reserved_sectors -
                            bs.number_of_fats * bs.fat_size_sectors -
                            bs.root_dir_entries * 32 / bs.sector_size)));
}

void print_tree(unsigned int cluster, int level) {
    Fat16Entry entry;
    char formatted_name[13];
    unsigned int dir_offset;
    long saved_pos;

    if (cluster == 0) {
        dir_offset = (pt[0].start_sector + bs.reserved_sectors + 
                     bs.number_of_fats * bs.fat_size_sectors) * bs.sector_size;
    } else {
        dir_offset = (pt[0].start_sector + bs.reserved_sectors + 
                     bs.number_of_fats * bs.fat_size_sectors +
                     bs.root_dir_entries * 32 / bs.sector_size +
                     (cluster - 2) * bs.sectors_per_cluster) * bs.sector_size;
    }

    fseek(in, dir_offset, SEEK_SET);

    int entries_to_read = (cluster == 0) ? bs.root_dir_entries : 
                         (bs.sectors_per_cluster * bs.sector_size) / sizeof(Fat16Entry);

    for (int i = 0; i < entries_to_read; i++) {
        saved_pos = ftell(in);
        
        if (fread(&entry, sizeof(entry), 1, in) != 1) break;
        
        if (entry.filename[0] == 0x00) break;
        if (entry.filename[0] == 0xE5) continue;
        if (entry.attributes & 0x08) continue;
        if (entry.filename[0] == '.') continue;

        format_filename(entry.filename, entry.ext, formatted_name);

        for (int j = 0; j < level; j++) printf("  ");
        printf("├── ");

        if (entry.attributes & 0x10) {
            printf("%s/\n", formatted_name);
            if (entry.starting_cluster != cluster) {
                print_tree(entry.starting_cluster, level + 1);
                fseek(in, saved_pos + sizeof(Fat16Entry), SEEK_SET);
            }
        } else {
            printf("%s (%u bytes)\n", formatted_name, entry.file_size);
        }
    }
}

void change_dir(char *path)
{
    printf("Changing directory to %s\n", path);
    for (int i = 0; path[i]; i++)
    {
        path[i] = toupper(path[i]);
    }

    char path_copy[256];
    strncpy(path_copy, path, sizeof(path_copy));
    path_copy[sizeof(path_copy) - 1] = '\0';
    char *token = strtok(path_copy, "/");

    while (token != NULL)
    {
        Fat16Entry entry;
        bool found = false;

        // If we're in root directory
        if (current_cluster == 0)
        {
            current_dir_offset = (pt[0].start_sector + bs.reserved_sectors +
                                  bs.number_of_fats * bs.fat_size_sectors) *
                                 bs.sector_size;
        }
        else
        {
            // Calculate directory offset from cluster
            current_dir_offset = (pt[0].start_sector + bs.reserved_sectors +
                                  bs.number_of_fats * bs.fat_size_sectors +
                                  bs.root_dir_entries * 32 / bs.sector_size +
                                  (current_cluster - 2) * bs.sectors_per_cluster) *
                                 bs.sector_size;
        }

        fseek(in, current_dir_offset, SEEK_SET);

        // Search for directory entry
        int entries_to_read = (current_cluster == 0) ? bs.root_dir_entries : (bs.sectors_per_cluster * bs.sector_size) / sizeof(Fat16Entry);

        for (int i = 0; i < entries_to_read; i++)
        {
            fread(&entry, sizeof(entry), 1, in);

            if (entry.filename[0] == 0x00)
                break;
            if (entry.filename[0] == 0xE5)
                continue;

            char formatted_name[13];
            format_filename(entry.filename, entry.ext, formatted_name);

            printf("Formatted name: %s\n", formatted_name);

            if (strcmp(formatted_name, token) == 0 && (entry.attributes & 0x10))
            {
                sprintf(current_path, "%s/%s", current_path, formatted_name);
                current_cluster = entry.starting_cluster;
                found = true;
                printf("Found directory %s at cluster %d\n", token, current_cluster);
                break;
            }
        }

        if (!found)
        {
            printf("Error: Directory %s not found\n", token);
            return;
        }

        token = strtok(NULL, "/");
    }
}

void load_fat()
{
    // Determine FAT size and allocate memory
    unsigned int fat_size_bytes = bs.fat_size_sectors * bs.sector_size;
    printf("Loading FAT table (%u bytes)\n", fat_size_bytes);
    fat_table = (unsigned int *)malloc(fat_size_bytes);

    if (fat_table == NULL)
    {
        printf("Error: Could not allocate memory for FAT table\n");
        exit(1);
    }

    // Seek to the FAT table
    unsigned int fat_offset = (pt[0].start_sector + bs.reserved_sectors) * bs.sector_size;
    fseek(in, fat_offset, SEEK_SET);

    // Read the FAT table
    fread(fat_table, 1, fat_size_bytes, in);
}

int read(const char *filename)
{
    Fat16Entry entry;
    char search_name[9], search_ext[4];
    bool found = false;

    // Parse filename into base and extension (convert to uppercase)
    memset(search_name, ' ', 8);
    memset(search_ext, ' ', 3);
    search_name[8] = search_ext[3] = '\0';

   char upper_filename[256];
    strncpy(upper_filename, filename, sizeof(upper_filename)-1);
    upper_filename[sizeof(upper_filename)-1] = '\0';
    for(int i = 0; upper_filename[i]; i++) {
        upper_filename[i] = toupper(upper_filename[i]);
    }

    // Seek to root directory
    unsigned int dir_offset;
    if (current_cluster == 0)
    {
        dir_offset = (pt[0].start_sector + bs.reserved_sectors +
                      bs.number_of_fats * bs.fat_size_sectors) *
                     bs.sector_size;
    }
    else
    {
        dir_offset = (pt[0].start_sector + bs.reserved_sectors +
                      bs.number_of_fats * bs.fat_size_sectors +
                      bs.root_dir_entries * 32 / bs.sector_size +
                      (current_cluster - 2) * bs.sectors_per_cluster) *
                     bs.sector_size;
    }
    fseek(in, dir_offset, SEEK_SET);

    // Find file in directory
    for (int i = 0; i < bs.root_dir_entries; i++) {
        fread(&entry, sizeof(entry), 1, in);

        if (entry.filename[0] == 0x00) break;
        if (entry.filename[0] == 0xE5) continue;
        if ((entry.attributes & 0x10) || (entry.attributes & 0x08)) continue;

        char formatted_name[13];
        format_filename(entry.filename, entry.ext, formatted_name);

        if (strcmp(formatted_name, upper_filename) == 0) {
            found = true;
            break;
        }
    }

    if (!found)
    {
        printf("Error: File not found\n");
        return -1;
    }

    if (fat_table == NULL)
        load_fat();

    // Calculation of data starting point
    unsigned int data_start = pt[0].start_sector + bs.reserved_sectors +
                              (bs.number_of_fats * bs.fat_size_sectors) +
                              ((bs.root_dir_entries * 32) + bs.sector_size - 1) / bs.sector_size;

    // Make buffer for one cluster
    unsigned char buffer[bs.sector_size * bs.sectors_per_cluster];

    // Read file cluster by cluster
    unsigned short cluster = entry.starting_cluster;
    unsigned int total_bytes_read = 0;

    // Open output file
    char output_filename[256];
    sprintf(output_filename, "output_%s", filename);
    FILE *output_file = fopen(output_filename, "wb");
    if (output_file == NULL)
    {
        printf("Error: Could not open output file %s\n", output_filename);
        return -1;
    }

    printf("Reading %s (%u bytes)...\n", filename, entry.file_size);

    while (cluster >= 0x0002 && cluster < 0xFFF0 && total_bytes_read < entry.file_size)
    {
        fseek(in, (data_start + (cluster - 2) * bs.sectors_per_cluster) * bs.sector_size, SEEK_SET);

        unsigned int bytes_to_read = bs.sector_size * bs.sectors_per_cluster;
        if (entry.file_size - total_bytes_read < bytes_to_read)
            bytes_to_read = entry.file_size - total_bytes_read;

        size_t bytes_actually_read = fread(buffer, 1, bytes_to_read, in);
        if (bytes_actually_read != bytes_to_read)
        {
            printf("Error: Could not read file data\n");
            fclose(output_file);
            return -1;
        }
        fwrite(buffer, 1, bytes_actually_read, output_file);

        total_bytes_read += bytes_actually_read;

        if (strstr(filename, ".JPG") || strstr(filename, ".jpg"))
        {
            printf(".");
        }
        else
        {
            printf("%.*s", (int)bytes_actually_read, buffer);
        }

        cluster = ((unsigned short *)fat_table)[cluster];
    }

    fclose(output_file);
    printf("\nFile saved to %s\n", output_filename);
    printf("Total bytes read: %u\n", total_bytes_read);

    return 0;
}

void write(char* filename){
    Fat16Entry new_entry;
    memset(&new_entry, 0, sizeof(Fat16Entry));
    
    char base[9], ext[4];
    memset(base, ' ', 8);
    memset(ext, ' ', 3);
    char* dot = strrchr(filename, '.');
    if (dot) {
        strncpy(base, filename, (dot - filename > 8) ? 8 : (dot - filename));
        strncpy(ext, dot + 1, 3);
    } else {
        strncpy(base, filename, 8);
    }
    printf("Base: %s, Ext: %s\n", base, ext);
    for(int i = 0; i < 8; i++) {
        new_entry.filename[i] = toupper(base[i]);
    }
    for(int i = 0; i < 3; i++) {
        new_entry.ext[i] = toupper(ext[i]);
    }

    printf("Filename: %.8s.%.3s\n", new_entry.filename, new_entry.ext);

    if(fat_table == NULL) {
        load_fat();
    }

    short current_cluster = 0xFFFF;

    for(unsigned short cluster = 2; cluster < (bs.fat_size_sectors * bs.sector_size / 2); cluster++) {
        if (((unsigned short*)fat_table)[cluster] == 0) {
            current_cluster = cluster;
            ((unsigned short*)fat_table)[current_cluster] = 0xFFFF;
            printf("Found starting cluster %d\n", current_cluster);
            break;
        }
    }

    FILE* file_to_write = fopen(filename, "rb+");
    if(file_to_write == NULL) {
        printf("Error: Could not open file %s\n", filename);
        return;
    }
    fseek(file_to_write, 0, SEEK_END);
    long file_size = ftell(file_to_write);
    fseek(file_to_write, 0, SEEK_SET);

    new_entry.attributes = 0x20;
    new_entry.file_size = file_size;
    new_entry.starting_cluster = current_cluster;

    //TODO - set current time and date
    new_entry.modify_time = 0;
    new_entry.modify_date = 0;

    unsigned int dir_offset = (pt[0].start_sector + bs.reserved_sectors +
        bs.number_of_fats * bs.fat_size_sectors) * bs.sector_size;

    Fat16Entry temp_entry;
    bool entry_written = false;
    for(int i = 0; i < bs.root_dir_entries; i++) {
        fseek(in, dir_offset + i * sizeof(Fat16Entry), SEEK_SET);
        fread(&temp_entry, sizeof(Fat16Entry), 1, in);

        if(temp_entry.filename[0] == 0x00 || temp_entry.filename[0] == 0xE5) {
            fseek(in, dir_offset + i * sizeof(Fat16Entry), SEEK_SET);
            fwrite(&new_entry, sizeof(Fat16Entry), 1, in);
            entry_written = true;
            printf("File created successfully\n");
            break;
        }
    }

    if (!entry_written) {
        printf("Error: No free directory entries\n");
        fclose(file_to_write);
        return;
    }

    unsigned char buffer[bs.sector_size * bs.sectors_per_cluster];
    unsigned int bytes_written = 0;

    unsigned int data_start = pt[0].start_sector + bs.reserved_sectors +
                              (bs.number_of_fats * bs.fat_size_sectors) +
                              ((bs.root_dir_entries * 32) + bs.sector_size - 1) / bs.sector_size;

    size_t bytes_read;
    printf("Writing %ld bytes in chunks of %d bytes\n", file_size, bs.sector_size * bs.sectors_per_cluster);
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file_to_write)) > 0) {
        // Write current chunk
        fseek(in, (data_start + (current_cluster - 2) * bs.sectors_per_cluster) * bs.sector_size, SEEK_SET);
        fwrite(buffer, 1, bytes_read, in);
        bytes_written += bytes_read;

        if (bytes_written < file_size) {
            unsigned short next_cluster = 0xFFFF;
            for(unsigned short cluster = 2; cluster < (bs.fat_size_sectors * bs.sector_size / 2); cluster++) {
                if (((unsigned short*)fat_table)[cluster] == 0) {
                    next_cluster = cluster;
                    ((unsigned short*)fat_table)[current_cluster] = next_cluster;
                    ((unsigned short*)fat_table)[next_cluster] = 0xFFFF;
                    current_cluster = next_cluster;
                    break;
                }
            }
        } else {
            ((unsigned short*)fat_table)[current_cluster] = 0xFFFF;
        }
    }

    unsigned int fat_offset = (pt[0].start_sector + bs.reserved_sectors) * bs.sector_size;
    for(int i = 0; i < bs.number_of_fats; i++) {
        fseek(in, fat_offset + i * bs.fat_size_sectors * bs.sector_size, SEEK_SET);
        fwrite(fat_table, bs.fat_size_sectors * bs.sector_size, 1, in);
    }

    if(bytes_written != file_size) {
        printf("Error: Not all bytes written\n");
        fclose(file_to_write);
        return;
    }

    fclose(file_to_write);
    printf("File created successfully\n");
}

void delete(char* filename) {
    if (fat_table == NULL) {
        load_fat();
    }
    Fat16Entry entry;
    char search_name[9], search_ext[4];
    bool found = false;
    unsigned int entry_offset = 0; 

    // Parse filename into base and extension (convert to uppercase)
    memset(search_name, ' ', 8);
    memset(search_ext, ' ', 3);
    search_name[8] = search_ext[3] = '\0';

   char upper_filename[256];
    strncpy(upper_filename, filename, sizeof(upper_filename)-1);
    upper_filename[sizeof(upper_filename)-1] = '\0';
    for(int i = 0; upper_filename[i]; i++) {
        upper_filename[i] = toupper(upper_filename[i]);
    }

    // Seek to root directory
    unsigned int dir_offset;
    if (current_cluster == 0)
    {
        dir_offset = (pt[0].start_sector + bs.reserved_sectors +
                      bs.number_of_fats * bs.fat_size_sectors) *
                     bs.sector_size;
    }
    else
    {
        dir_offset = (pt[0].start_sector + bs.reserved_sectors +
                      bs.number_of_fats * bs.fat_size_sectors +
                      bs.root_dir_entries * 32 / bs.sector_size +
                      (current_cluster - 2) * bs.sectors_per_cluster) *
                     bs.sector_size;
    }
    fseek(in, dir_offset, SEEK_SET);

    for (int i = 0; i < bs.root_dir_entries; i++) {
        entry_offset = dir_offset + i * sizeof(Fat16Entry);
        fseek(in, entry_offset, SEEK_SET);
        fread(&entry, sizeof(entry), 1, in);

        if (entry.filename[0] == 0x00) break;
        if (entry.filename[0] == 0xE5) continue;
        if ((entry.attributes & 0x10) || (entry.attributes & 0x08)) continue;

        char formatted_name[13];
        format_filename(entry.filename, entry.ext, formatted_name);

        if (strcmp(formatted_name, upper_filename) == 0) {
            found = true;
            break;
        }
    }

    if (!found)
    {
        printf("Error: File not found\n");
        return;
    }

    // // Mark file as deleted in directory
    fseek(in, entry_offset, SEEK_SET);
    unsigned char deleted = 0xE5;
    fwrite(&deleted, 1, 1, in);

    unsigned short cluster = entry.starting_cluster;
    while(cluster >= 0x0002 && cluster < 0xFFF0) {
        unsigned short next_cluster = ((unsigned short*)fat_table)[cluster];
        ((unsigned short*)fat_table)[cluster] = 0x0000;  // Mark as free
        cluster = next_cluster;
    }

    unsigned int fat_offset = (pt[0].start_sector + bs.reserved_sectors) * bs.sector_size;
    for(int i = 0; i < bs.number_of_fats; i++) {
        fseek(in, fat_offset + i * bs.fat_size_sectors * bs.sector_size, SEEK_SET);
        fwrite(fat_table, bs.fat_size_sectors * bs.sector_size, 1, in);
    }

    printf("File %s deleted successfully\n", filename);
}

int main(int argc, char **argv)
{
    in = fopen("sd.img", "rb+");
    int i;

    // PartitionTable pt[4];
    // //Fat16BootSector bs;
    Fat16Entry entry;

    fseek(in, 0x1BE, SEEK_SET);               // go to partition table start, partitions start at offset 0x1BE, see http://www.cse.scu.edu/~tschwarz/coen252_07Fall/Lectures/HDPartitions.html
    fread(pt, sizeof(PartitionTable), 4, in); // read all entries (4)

    printf("Partition table\n-----------------------\n");
    for (i = 0; i < 4; i++){ // for all partition entries print basic info
        printf("Partition %d, type %02X, ", i, pt[i].partition_type);
        printf("start sector %8d, length %8d sectors\n", pt[i].start_sector, pt[i].length_sectors);
    }

    printf("\nSeeking to first partition by %d sectors\n", pt[0].start_sector);
    fseek(in, 512 * pt[0].start_sector, SEEK_SET); // Boot sector starts here (seek in bytes)
    fread(&bs, sizeof(Fat16BootSector), 1, in);    // Read boot sector content, see http://www.tavi.co.uk/phobos/fat.html#boot_block
    printf("Volume_label %.11s, %d sectors size\n", bs.volume_label, bs.sector_size);

    // Seek to the beginning of root directory, it's position is fixed
    fseek(in, (bs.reserved_sectors - 1 + bs.fat_size_sectors * bs.number_of_fats) * bs.sector_size, SEEK_CUR);

    // Read all entries of root directory
    printf("\nFilesystem root directory listing\n-----------------------\n");
    for (i = 0; i < bs.root_dir_entries; i++){
        fread(&entry, sizeof(entry), 1, in);
        // Skip if filename was never used, see http://www.tavi.co.uk/phobos/fat.html#file_attributes
        if (entry.filename[0] != 0x00)
        {
            printf("%.8s.%.3s attributes 0x%02X starting cluster %8d len %8d B\n", entry.filename, entry.ext, entry.attributes, entry.starting_cluster, entry.file_size);
            print_directory();
        }
    }

    if (argc == 2)
    {
        printf("\nReading %s:\n-----------------------\n", argv[1]);
        if (read(argv[1]) == -1)
        {
            printf("Error: File not found or couldn't be read\n");
        }
    }

    char input[256];
    while (true)
    {
        printf("%s>", current_path); // Add prompt
        fgets(input, 256, stdin);
        input[strcspn(input, "\n")] = 0;

        if (strncmp(input, "ls", 2) == 0)
        {
            print_directory();
        }
        else if (strncmp(input, "exit", 4) == 0)
        {
            break;
        }
        else if (strncmp(input, "cd ", 3) == 0)
        {
            if (strcmp(input + 3, "..") == 0)
            {
                printf("Moving up to parent directory\n");
                current_cluster = 0; // Reset to root directory
                char* last_slash = strrchr(current_path, '/');
                if (last_slash != NULL && last_slash != current_path) {
                    *last_slash = '\0';  // Truncate string at last slash
                }
                
                // If we removed everything, restore root
                if (strlen(current_path) == 0) {
                    strcpy(current_path, "Groot");
                }
            }
            else if (strcmp(input + 3, ".") == 0)
            {
                printf("Staying in current directory\n");
            }
            else
            {
                printf("Changing directory to %s\n", input + 3);
                change_dir(input + 3);
            }
        }
        else if (strncmp(input, "read ", 5) == 0)
        {
            read(input + 5);
        }
        else if(strncmp(input, "write ", 6) == 0)
        {
            printf("Creating file %s\n", input + 6);
            write(input + 6);
        }
        else if (strncmp(input, "del ", 4) == 0)
        {
            printf("Deleting file %s\n", input + 4);
            delete(input + 4);
        }
        else if (strncmp(input, "help", 4) == 0)
        {
            printf("Available commands:\n");
            printf("  ls           - List directory contents\n");
            printf("  cd <dir>     - Change directory\n");
            printf("  read <file>  - Read file contents\n");
            printf("  help         - Show this help message\n");
            printf("  tree         - Show directory tree\n");
            printf("  write <file> - Create a new file\n");
            printf("  del <file>   - Delete a file\n");
            printf("  exit         - Exit program\n");
        }
        else if(strncmp(input, "tree", 4) == 0)
        {
           // print_tree();
            print_tree(0, 1);
        }
        else
        {
            printf("Unknown command: %s\n", input);
        }
        printf("\n");
    }

    free(fat_table);
    fclose(in);
    return 0;
}