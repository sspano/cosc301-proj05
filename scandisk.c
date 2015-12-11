#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdbool.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

const int NUM_CLUSTERS = 2880;
int ref_count[NUM_CLUSTERS] = {0}; 
const int ORPHANED = -42;
bool onetime = false;


void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}

void print_indent(int indent)
{
    int i;
    for (i = 0; i < indent*4; i++)
	printf(" ");
}

 // update orphan function
void update_annie(struct bpb33 *bpb,uint8_t *image_buf){
    
    for (int i = 33; i < NUM_CLUSTERS; i++){
        if (ref_count[i] == ORPHANED){
            printf("Found Orphan! Cluster #: %d\n", i);
            set_fat_entry(i, FAT12_MASK & CLUST_FREE, image_buf, bpb);
            ref_count[i] = 0;
            
         //   create_dirent(i, image_buf, bpb);

        }
        else if (ref_count[i] == 0 && ((FAT12_MASK & CLUST_FREE) != get_fat_entry(i, image_buf, bpb))){
            printf("Found enslaved (not free) Orphan! Yay! Another friend for Harry! Cluster #: %d\n", i);
           set_fat_entry(i, FAT12_MASK & CLUST_FREE, image_buf, bpb);
            
            //create_dirent(i, image_buf, bpb);
        }
        else {
           // printf("I'm okay! Cluster # %d\n", i);
        }
    }
}

bool check_size(struct direntry *dirent, struct bpb33 *bpb, uint8_t *image_buf){
    int meta_size = getulong(dirent->deFileSize);
    bool has_size_problem = false;
    uint16_t cluster_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
    uint32_t fat_size = 0; // why did why initialize to cluster size?
    uint16_t cluster = getushort(dirent->deStartCluster);
    uint16_t prev_cluster = cluster;

    //printf("\tCluster Size (%d bytes)\n",(int) cluster_size);

    while(is_valid_cluster(cluster, bpb)){
        fat_size +=cluster_size;
        ref_count[cluster] += 1;
        prev_cluster = cluster;
        cluster = get_fat_entry(prev_cluster, image_buf, bpb);
       
     
        if(fat_size>meta_size && meta_size+cluster_size > fat_size){ //beyond meta_size, set eof
            set_fat_entry(prev_cluster, FAT12_MASK & CLUST_EOFS, image_buf, bpb);
        } else if (fat_size> meta_size){ //free it
            ref_count[prev_cluster] = ORPHANED; 
            has_size_problem = true;
        }
    }

    printf("\tCalculated FAT size: %d\n", fat_size);
    printf("\tMeta Size - FAT size: %d\n",(int) (meta_size - fat_size));
    
    // NEW TEST BEGIN
    if (has_size_problem){
        int fat_size1 = 0;
        cluster = getushort(dirent->deStartCluster);
        while(is_valid_cluster(cluster, bpb)){
            fat_size1 +=cluster_size;
            prev_cluster = cluster;
            cluster = get_fat_entry(prev_cluster, image_buf, bpb);
        }
        printf("\tFat size after fix: (%d bytes)\n", fat_size1);
   
    }

 // NEW TEST END

  
    
    //check conditions for meta_size in approrpiate range for cluster_size*num_clust
    if (meta_size > fat_size){
        printf("\tMetasize Before: %d\n", meta_size);
        putulong(dirent->deFileSize, fat_size);
        printf("\tMetasize After: %d\n", getulong(dirent->deFileSize));
        has_size_problem = true;
        return has_size_problem;
    } else { //no modifications necessary
        return has_size_problem; 
    }

}

uint16_t access_dirent(struct direntry *dirent, int indent, struct bpb33 *bpb, uint8_t *image_buf)
{
    uint16_t followclust = 0;

    int i;
    char name[9];
    char extension[4];
    uint32_t size;
    uint16_t file_cluster;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);
    if (name[0] == SLOT_EMPTY)
    {
	return followclust;
    }

    /* skip over deleted entries */
    if (((uint8_t)name[0]) == SLOT_DELETED)
    {
	return followclust;
    }

    if (((uint8_t)name[0]) == 0x2E)
    {
	// dot entry ("." or "..")
	// skip it
        return followclust;
    }

    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--) 
    {
	if (name[i] == ' ') 
	    name[i] = '\0';
	else 
	    break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--) 
    {
	if (extension[i] == ' ') 
	    extension[i] = '\0';
	else 
	    break;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN)
    {
	// ignore any long file name extension entries
	//
	// printf("Win95 long-filename entry seq 0x%0x\n", dirent->deName[0]);
    }
    else if ((dirent->deAttributes & ATTR_VOLUME) != 0) 
    {
	printf("Volume: %s\n", name);
    } 
    else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) 
    {
        // don't deal with hidden directories; MacOS makes these
        // for trash directories and such; just ignore them.
	if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN)
        {
	    print_indent(indent);
    	    printf("%s/ (directory)\n", name);
            file_cluster = getushort(dirent->deStartCluster);
            followclust = file_cluster;
            ref_count[(int) file_cluster] += 1;
        }
    }
    else 
    {
        /*
         * a "regular" file entry
         * print attributes, size, starting cluster, etc.
         */
	int ro = (dirent->deAttributes & ATTR_READONLY) == ATTR_READONLY;
	int hidden = (dirent->deAttributes & ATTR_HIDDEN) == ATTR_HIDDEN;
	int sys = (dirent->deAttributes & ATTR_SYSTEM) == ATTR_SYSTEM;
	int arch = (dirent->deAttributes & ATTR_ARCHIVE) == ATTR_ARCHIVE;

	size = getulong(dirent->deFileSize);
	print_indent(indent);
        printf("%s.%s (%u bytes) (starting cluster %d) %c%c%c%c\n", 
	       name, extension, size, getushort(dirent->deStartCluster),
	       ro?'r':' ', 
               hidden?'h':' ', 
               sys?'s':' ', 
               arch?'a':' ');
        
        print_indent(indent);
        //check for size inconsistencies
        bool had_size_problem  = check_size(dirent, bpb, image_buf);

        if(had_size_problem){
	   printf("Detected size problem with %s.%s (%u bytes) (starting cluster %d) %c%c%c%c\n", 
	       name, extension, getulong(dirent->deFileSize), getushort(dirent->deStartCluster),
	       ro?'r':' ', 
               hidden?'h':' ', 
               sys?'s':' ', 
               arch?'a':' ');
        }
    }

    return followclust;
}

void follow_dir(uint16_t cluster, int indent,
		uint8_t *image_buf, struct bpb33* bpb)
{
    while (is_valid_cluster(cluster, bpb))
    {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
	for ( ; i < numDirEntries; i++)
	{
            
            uint16_t followclust = access_dirent(dirent, indent, bpb, image_buf);
            if (followclust)
                follow_dir(followclust, indent+1, image_buf, bpb);
            dirent++;
	}

	cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}


void traverse_root(uint8_t *image_buf, struct bpb33* bpb)
{
    uint16_t cluster = 0;

    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

    int i = 0;
    for ( ; i < bpb->bpbRootDirEnts; i++)
    {
        uint16_t followclust = access_dirent(dirent, 0, bpb, image_buf);
        if (is_valid_cluster(followclust, bpb))
            follow_dir(followclust, 1, image_buf, bpb);

        dirent++;
    }
}

int main(int argc, char** argv){  
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;
    if (argc < 2) {
	usage(argv[0]);
    }

    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);

    // your code should start here...

    //check filesize in metadata vs. FAT clusters

    //print out a list of files whose length is inconsistent
    //free clusters beyond EOF as defined in metadata
    //adjust size in metadata if EOF or BAD before end of data according to metadata
    
    //deFileSize in direntry
    //get_fat_entry until EOF or BAD to find cluster size to compare


    traverse_root(image_buf, bpb); //this call will execute checks inside print_dirent
 // update orphan function
    update_annie(bpb,image_buf);


    unmmap_file(image_buf, &fd);
    return 0;
}
