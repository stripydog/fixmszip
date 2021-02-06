/* fixmszip.c.  Simple program to fix large zip files created on
 * Windows so that mac/unix zip utilities play nicely with them.
 * This involves changing the "Total Number of disks" field in the Zip64
 * End Of Central Directory Locator structure from "0" to "1"
 * Use is entirely at user's own risk
 * Copyright Keith Young 2021
 * For copying information, see the file COPYING distributed with this file
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

#define EOCDR_BASE_SIZE 22          /* End of Central Directory record size */
#define Z64_EOCDL_SIZE 20           /* Zip64 EOCD Locator */
#define Z64_EOCDL_SIG 0x07064b50    /* Zip64 EOCDL signature */
#define SIG_LEN 4                   /* Zip64 signature length */

static char *progname = "fixmszip";
static int little_endian;           /* This system's endianness. 1 == litle */

/* Print usage an exit */
void usage()
{
    fprintf(stderr,"Usage: %s [-v] zipfile [...]\n",progname);
    exit(1);
}

/* Return 2 little endian bytes as an unsigned short */
unsigned short get2bytes(const unsigned char* ptr)
{
    if (little_endian) {
        return((*(ptr+1) <<8) + *ptr);
    }

    return((*ptr <<8) + *(ptr+1));
}

/* Return 4 little endian bytes as an unsigned int */
unsigned int get4bytes(const unsigned char *ptr)
{
    int i;
    unsigned int val=0;

    if (little_endian) {
        for (i = 0; i < 4; i++) {
            val += *ptr++ <<(8 * i);
        }
    } else {
        for (i = 0; i < 4; i++) {
            val += *ptr++ <<(8* (3-i));
        }
    }
    return val;
}

/* Find and patch a problematic Zip64 EOCDL

   We do this as follows.
   * mmap enough of the last part of the file to encompass the Zip64 EOCDL
     and the EOCDR including the maximum sized comment, and rouded up to the
     previous page boundary
   * Starting from 18 bytes from the end of the file (where the last byte of
     the EOCDR signature would be if there were no comment), look backwards
     through the file for the signature.  If found, check that the comment
     length in the last two bytes of the EOCDR (assuming the signature does
     mark the start of th EOCDR), added to the offset of the assumed end of the
     EOCDR is equal to the file size.  If not, the signature is a fluke, so keep
     looking.  If the value looks "correct" check that:
     - This disk is the start disk (from fields in the EOCDR). We ignore this
       file if not.
     - That the central directory offset is 0xffffffff, signifying a Zip64 file.
       If not, the file doesn't need patching
     - That the Zip64 EOCDL signature is where it should be
     - That the Zip64 EOCDL "Total number of disks" field is set to 0
     ...and assuming the "dryrun" option is not set, change number of disks to 1

    return values: -1: error
                    0: file doesn't need updating
                    1: file updated (or would have been if not dryrun)
*/
int fixup(char *filename, int dryrun)
{
    int fd,i,res=0,pagesize;
    unsigned char *fptr,*ptr,*minptr;
    off_t fsize,offsize = 0, pageoff;
    struct stat sbuf;
    unsigned cd_offset,z64sig,numdisks;
    unsigned short comment_len,this_disk,start_disk;
    const unsigned char sig[] = {0x50,0x4b,0x05,0x06};

    if (lstat(filename,&sbuf)) {
        fprintf(stderr,"Failed to stat %s: %s\n",filename,strerror(errno));
        return(-1);
    }

    /* If file is smaller than End of Central Directory Record, it can't be
       a zip file (TODO: check for smallest viable zip file */
    if ((fsize = sbuf.st_size) < EOCDR_BASE_SIZE) {
        fprintf(stderr,"%s is not a zip file\n",filename);
        return(-1);
    }

    /* We don't need to mmap all of the file, only enough of the last part
       to encompass the End of Central Directory Record (EOCDR) including
       any comment and the Zip64 End of Central Directory Locator (EOCDL),
       plus enough preceding bytes to enable mapping on a page boundary. */
    if (fsize > 65577) {
        offsize = fsize - 65577;
        pagesize = getpagesize();
        pageoff = offsize % pagesize;
        offsize -= pageoff;
        fsize = 65577;
    } else {
        offsize = pageoff = 0;
    }

    if ((fd = open(filename,O_RDWR)) < 0) {
        fprintf(stderr,"Failed to open %s: %s\n",filename,strerror(errno));
        return(-1);
    }

    if ((fptr = (unsigned char *) mmap(NULL,fsize,PROT_READ|PROT_WRITE,
                MAP_SHARED,fd,offsize)) == MAP_FAILED) {
        (void) close(fd);
        fprintf(stderr,"Failed to mmap %s: %s\n",filename,strerror(errno));
        return(-1);
    }

    /* Starting at what would be the last byte of the EOCDR signature were
       there no comment, work backwards through the mmaped part of the file
       looking for the EOCDR signature */
    for (i=3,ptr=fptr+fsize+pageoff-EOCDR_BASE_SIZE+3,
                minptr=fptr+pageoff+Z64_EOCDL_SIZE;ptr >= minptr ;--ptr) {
        if (*ptr == sig[i]) {
            if (i == 0) {
                /* If we think we've found the signature, check what would
                   then be the comment length field and confirm that offset
                   of the EOCDR, plus length of the EOCDR, plus comment length
                   are equal to the file size.  If not, signature must be a
                   fluke: continute searching. */
                comment_len = get2bytes(ptr+20);
                if ((ptr - fptr) - pageoff + EOCDR_BASE_SIZE + comment_len
                        != fsize) {
                    i = SIG_LEN;
                    continue;
                }

                /* Here it looks like we've found the EOCDR.  If this disk
                   is not the start disk, we don't do anything */
                this_disk = get2bytes(ptr+4);
                start_disk = get2bytes(ptr+6);
                if (this_disk != start_disk) {
                    fprintf(stderr,"Not start disk\n");
                    res = -1;
                    break;
                }

                /* If the central directory offset is not 0xffffffff, there
                   should be no need to patch the Zip64 EOCDL */
                cd_offset = get4bytes(ptr+16);
                if (cd_offset != 0xffffffff) {
                    break;
                }

                /* Check the Zip64 EOCDL signature is where it should be */
                z64sig = get4bytes(ptr-Z64_EOCDL_SIZE);
                if (z64sig != Z64_EOCDL_SIG) {
                    i = SIG_LEN;
                    continue;
                }

                /* Check number of disks in Zip64 EOCDL.  If 0 and not
                   a dry run, change it to 1 */
                numdisks = get4bytes(ptr-4);
                if (numdisks == 0) {
                    if (!dryrun) {
                        *(ptr-4) = 1;
                    }
                    res = 1;
                }
                break;
            } else {
                --i;
            }
        } else {
            if (i != 3) {
                i = 3;
            }
        }
    }
    (void) munmap(fptr,fsize);
    return(res);
}

int main (int argc, char **argv)
{
    unsigned problems = 0;
    int c,i,err,res,verbose = 0,nopatch=0;

    c = 1;
    little_endian =  *(char *)&c;

    while ((c = getopt(argc,argv,"vn")) != -1) {
        switch (c) {
        case 'v':       /* Verbose output */
            verbose++;
            break;
        case 'n':       /* dry run */
            nopatch++;
            break;
        default:
            usage();
        }
    }

    if (optind == argc) {
        usage();
    }

    for (i = optind; i < argc; i++) {
        if (verbose) {
            printf("Fixing %s...",argv[i]);
        }
        if (access(argv[i],W_OK)) {
            err=errno;
            if (verbose) {
                printf("Failed!\n");
                fflush(stdout);
            }
            fprintf(stderr,"Failed to fix %s: %s\n",argv[i],strerror(err));
            fflush(stderr);
            problems++;
            continue;
        }

        if ((res = fixup(argv[i],nopatch)) < 0) {
            problems++;
        }

        if (verbose) {
            if (res == 1) {
                printf("Succeeded\n");
            } else if (res == 0) {
                printf("Unnecessary\n");
            } else {
                printf("Failed\n");
            }
            fflush(stdout);
            fflush(stderr);
        }
    }

    if (problems) {
        fprintf(stderr,"Errors were encountered during fixup\n");
        exit(1);
    }
    exit(0);
}

