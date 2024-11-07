#include "fsck.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

dirent *get_next_free_dirent(dinode *ip, int n);
bool has_directory_cycle(int inum);
dinode *get_nth_inode(int n);
dirent *get_nth_dirent(dinode *ip, int n);
void set_nth_bit_1(u8 *bitmap, int n);
void set_nth_bit_0(u8 *bitmap, int n);
bool is_addr_in_bounds(u32 addr);
bool is_nth_bit_1(u8 *bitmap, int n);

u8 tdir_inodes_bitmap[INODEBMAPSIZE] = {0};

u8 *file_bytes;
struct stat statbuf;
bool repair_mode;
bool repair_required = false;

void error(char *error_msg) {
  fprintf(stderr, "%s\n", error_msg);
  if (!repair_mode) {
    munmap(file_bytes, statbuf.st_size);
    exit(1);
  }
}

int main(int argc, char *argv[]) {
  int fd;
  int file_image_index;

  if (argc == 2) {
    repair_mode = false;
    file_image_index = 1;
  } else if (argc == 3 && (strcmp(argv[1], "-r") == 0)) {
    repair_mode = true;
    file_image_index = 2;
  } else {
    fprintf(stderr, "usage: fsck [-r] <file_system_image>\n");
    exit(1);
  }

  int oflag = repair_mode ? O_RDWR : O_RDONLY;

  fd = open(argv[file_image_index], oflag);
  if (fd == -1) {
    if (errno == ENOENT) {
      fprintf(stderr, "image not found\n");
      exit(1);
    }
    perror("could not open image");
  }

  fstat(fd, &statbuf);
  assert(fstat(fd, &statbuf) == 0);

  int prot = repair_mode ? PROT_READ | PROT_WRITE : PROT_READ;
  int flags = repair_mode ? MAP_SHARED : MAP_PRIVATE;

  file_bytes = mmap(NULL, statbuf.st_size, prot, flags, fd, 0);
  assert(file_bytes != MAP_FAILED);

  for (int i = 0; i < NINODES; i++) {
    dinode *ip = get_nth_inode(i);
    u16 type = xshort(ip->type);
    switch (type) {
    case 0:
    case T_DIR:
    case T_FILE:
    case T_DEV:
      break;
    default:
      error("ERROR: bad inode.");
    }
  }

  u32 direct_addr_references[FSSIZE] = {0};
  u32 indirect_addr_references[FSSIZE] = {0};
  u8 used_inodes_bitmap[INODEBMAPSIZE] = {0};
  u8 inode_references[NINODES] = {0};

  inode_references[1]++;
  for (int i = 0; i < NINODES; i++) {
    dinode *ip = get_nth_inode(i);
    u16 type = xshort(ip->type);
    if (type == 0)
      continue;

    if (type == T_DIR) {
      set_nth_bit_1(tdir_inodes_bitmap, i);
      for (int j = 2; j < NDIRENT; j++) {
        dirent *de = get_nth_dirent(ip, j);
        u16 inum = xshort(de->inum);
        if (inum != 0)
          inode_references[inum]++;
      }
    }

    set_nth_bit_1(used_inodes_bitmap, i);

    for (int j = 0; j < NDIRECT; j++) {
      u16 direct_addr = xshort(ip->addrs[j]);
      if (direct_addr != 0)
        direct_addr_references[direct_addr] += 1;
    }

    // address to the indirect address block is considered a direct address
    u32 indirect_block_addr = xint(ip->addrs[NDIRECT]);
    direct_addr_references[indirect_block_addr] += 1;

    // if the address is out of bounds and not equal to 0, error will be
    // thrown later
    if (is_addr_in_bounds(indirect_block_addr)) {
      u32 *indirect_block = (u32 *)(file_bytes + indirect_block_addr * BSIZE);
      for (int j = 0; j < NINDIRECT; j++) {
        u32 indirect_addr = xint(indirect_block[j]);
        if (indirect_addr != 0)
          indirect_addr_references[indirect_addr] += 1;
      }
    }
  }

  for (int addr = 0; addr < FSSIZE; addr++) {
    if (direct_addr_references[addr] == 0)
      continue;
    if (!is_addr_in_bounds(addr) && addr != 0) {
      error("ERROR: bad direct address in inode.");
    }
  }

  for (int addr = 0; addr < FSSIZE; addr++) {
    if (indirect_addr_references[addr] == 0)
      continue;
    if (!is_addr_in_bounds(addr) && addr != 0) {
      error("ERROR: bad indirect address in inode.");
    }
  }

  dinode *root_ip = get_nth_inode(1);
  if (xshort(root_ip->type) != T_DIR)
    error("ERROR: root directory does not exist");

  dirent *root_self_dirent = get_nth_dirent(root_ip, 0);
  dirent *root_parent_dirent = root_self_dirent + 1;
  u16 root_self_inum = xshort(root_self_dirent->inum);
  u16 root_parent_inum = xshort(root_parent_dirent->inum);

  if (root_self_inum != 1 || root_parent_inum != 1)
    error("ERROR: root directory does not exist");

  for (int i = 0; i < NINODES; i++) {
    dinode *ip = get_nth_inode(i);
    u16 type = xshort(ip->type);
    if (type == T_DIR) {
      dirent *dp0 = get_nth_dirent(ip, 0);
      u16 inum = xshort(dp0->inum);
      char *self_name = dp0->name;
      dirent *dp1 = get_nth_dirent(ip, 1);
      char *parent_name = dp1->name;

      if (inum != i || strcmp(self_name, ".") != 0 ||
          strcmp(parent_name, "..") != 0)
        error("ERROR: directory not properly formatted");
    }
  }

  u8 *bitmap = file_bytes + BMAPSTART * BSIZE;
  for (int addr = 0; addr < FSSIZE; addr++) {
    if (direct_addr_references[addr] == 0)
      continue;
    if (!is_nth_bit_1(bitmap, addr)) {
      error("ERROR: address used by inode but marked free in bitmap.");
    }
  }
  for (int addr = 0; addr < FSSIZE; addr++) {
    if (indirect_addr_references[addr] == 0)
      continue;
    if (!is_nth_bit_1(bitmap, addr)) {
      error("ERROR: address used by inode but marked free in bitmap.");
    }
  }

  u8 used_bitmap[BMAPSIZE];
  memcpy(used_bitmap, bitmap, BMAPSIZE);
  for (int addr = 0; addr < FSSIZE; addr++) {
    if (direct_addr_references[addr] == 0)
      continue;
    set_nth_bit_0(used_bitmap, addr);
  }
  for (int addr = 0; addr < FSSIZE; addr++) {
    if (indirect_addr_references[addr] == 0)
      continue;
    set_nth_bit_0(used_bitmap, addr);
  }

  // I have no idea what this condition is checking for
  if (used_bitmap[7] != 0x07)
    error("ERROR: bitmap marks block in use but it is not in use");

  for (int i = 8; i < BMAPSIZE; i++) {
    if (used_bitmap[i] != 0)
      error("ERROR: bitmap marks block in use but it is not in use");
  }

  for (int addr = 0; addr < FSSIZE; addr++) {
    if (direct_addr_references[addr] > 1 ||
        (direct_addr_references[addr] == 1 &&
         indirect_addr_references[addr] > 0))
      error("ERROR: direct address used more than once");
  }

  for (int addr = 0; addr < FSSIZE; addr++) {
    if (indirect_addr_references[addr] > 1)
      error("ERROR: indirect address used more than once");
  }

  for (int i = 0; i < NINODES; i++) {
    bool nth_inode_used = is_nth_bit_1(used_inodes_bitmap, i);
    bool nth_inode_referenced = inode_references[i] > 0;
    if (nth_inode_used && nth_inode_referenced)
      // Referenced inodes are removed from the bitmap. The remaining bitmap
      // stores the used inodes that have not been referenced. In repair mode,
      // these remainging inodes are placed in the lost_found directory
      set_nth_bit_0(used_inodes_bitmap, i);
    if (nth_inode_used && !nth_inode_referenced) {
      error("ERROR: inode marked use but not found in a directory.");
      repair_required = true;
    }
    if (!nth_inode_used && nth_inode_referenced)
      error("ERROR: inode referred to in a directory but marked free");
  }

  for (int i = 0; i < NINODES; i++) {
    dinode *ip = get_nth_inode(i);
    if (xshort(ip->type) == T_FILE && xshort(ip->nlink) != inode_references[i])
      error("ERROR: bad reference count for file");
  }

  for (int i = 0; i < NINODES; i++) {
    dinode *ip = get_nth_inode(i);
    if (xshort(ip->type) == T_DIR &&
        (xshort(ip->nlink) > 1 || inode_references[i] > 1))
      error("ERROR: directory appears more than once in the system");
  }

  // skipping dummy inode and root inode
  for (int i = 2; i < NINODES; i++) {
    dinode *ip = get_nth_inode(i);
    if (xshort(ip->type) != T_DIR)
      continue;
    dirent *parent_dirent = get_nth_dirent(ip, 1);
    dinode *parent_ip = get_nth_inode(xshort(parent_dirent->inum));
    if (xshort(parent_ip->type) != T_DIR)
      error("ERROR: parent of directory is not a directory");

    bool parent_directory_match = false;
    for (int j = 2; j < NDIRENT; j++) {
      dirent *de = get_nth_dirent(parent_ip, j);
      if (xshort(de->inum) == i)
        parent_directory_match = true;
    }
    if (!parent_directory_match)
      error("ERROR: parent directory mismatch");
  }

  // 1 is inum of the root directory
  if (has_directory_cycle(1))
    error("ERROR: file directory contains a cycle");

  for (int i = 0; i < INODEBMAPSIZE; i++) {
    if (tdir_inodes_bitmap[i] != 0)
      error("ERROR: inaccessible directory exists.");
  }

  if (!repair_mode) {
    munmap(file_bytes, statbuf.st_size);
    return 0;
  }

  if (!repair_required) {
    printf("file system image is valid, no repair required\n");
    munmap(file_bytes, statbuf.st_size);
    return 0;
  }

  dinode *lost_found_inode = NULL;

  for (int i = 0; i < NDIRENT; i++) {
    dirent *de = get_nth_dirent(root_ip, i);
    if (strcmp(de->name, "lost_found") == 0) {
      lost_found_inode = get_nth_inode(xshort(de->inum));
      break;
    }
  }

  if (!lost_found_inode) {
    fprintf(
        stderr,
        "ERROR: root directory does not contain the lost_found directory\n");
    munmap(file_bytes, statbuf.st_size);
    exit(1);
  }

  int lf_dirent_start_index = 0;

  for (int i = 0; i < NINODES; i++) {
    if (!is_nth_bit_1(used_inodes_bitmap, i))
      continue;
    dirent *de = get_next_free_dirent(lost_found_inode, lf_dirent_start_index);

    if (!de) {
      fprintf(stderr, "ERROR: lost_found directory is full, unable to repair "
                      "file system image\n");
      munmap(file_bytes, statbuf.st_size);
      exit(1);
    }
    de->inum = xshort(i);
  }

  munmap(file_bytes, statbuf.st_size);
  return 0;
}

bool has_directory_cycle(int inum) {
  dinode *ip = get_nth_inode(inum);
  if (xshort(ip->type) != T_DIR)
    return false;
  if (!is_nth_bit_1(tdir_inodes_bitmap, inum))
    return true;
  set_nth_bit_0(tdir_inodes_bitmap, inum);
  for (int i = 2; i < NDIRENT; i++) {
    dirent *de = get_nth_dirent(ip, i);
    if (has_directory_cycle(xshort(de->inum)))
      return true;
  }
  return false;
}

dirent *get_next_free_dirent(dinode *ip, int n) {
  if (n == NDIRENT)
    return NULL;

  dirent *de = get_nth_dirent(ip, n);
  while (n < NDIRENT) {
    if (xshort(de->inum) == 0)
      return de;
    de++;
    n++;
  }
  return NULL;
}

dinode *get_nth_inode(int n) {
  assert(n >= 0 && n < NINODES);
  return (dinode *)(file_bytes + INODESTART * BSIZE + n * sizeof(dinode));
}

dirent *get_nth_dirent(dinode *ip, int n) {
  assert(n >= 0 && n < NDIRENT);
  return (dirent *)(file_bytes + xshort(ip->addrs[0]) * BSIZE +
                    n * sizeof(dirent));
}

void set_nth_bit_1(u8 *bitmap, int n) { bitmap[n / 8] |= (1 << (n % 8)); }
void set_nth_bit_0(u8 *bitmap, int n) { bitmap[n / 8] &= ~(1 << (n % 8)); }

bool is_nth_bit_1(u8 *bitmap, int n) {
  u8 byte = bitmap[n / 8];
  return (byte & (1 << (n % 8))) > 0;
}

bool is_addr_in_bounds(u32 addr) { return addr >= DATASTART && addr < FSSIZE; }
