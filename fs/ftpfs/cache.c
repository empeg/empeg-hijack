#define __NO_VERSION__

#include<linux/version.h> 
#include<linux/config.h>
#include<linux/kernel.h>
#include<linux/module.h> 
#include<linux/fs.h>
#include<linux/malloc.h>
#include<linux/locks.h>
#include<linux/string.h> 

#include<asm/uaccess.h>

#include "ftpfs.h"  

static struct ftp_dir_cache dir_cache;

int
ftp_cache_init(){
	memset(&dir_cache, 0, sizeof(dir_cache));
	return 0;
}

unsigned long
ftp_cache_hash(char* name){
	unsigned long hash = 0;
	int i;

	for(i=0; i<strlen(name); i++)
		hash += name[i];
	return hash % FTP_CACHEHASH;
}

int
ftp_cache_deldir(struct ftp_hashlist_node* n){
	struct ftp_dirlist_node *p,*q;

	DEBUG(" aha, aha\n");
	if(!n) return -1;
	
	for(p = n->directory.head; p!=NULL; p = q){
		q = p->next;
		if(p->entry.name!=NULL) {
			kfree(p->entry.name);
			p->entry.name = NULL;
		}
		kfree(p);
	}

	if(n->directory.name!=NULL) 
		kfree(n->directory.name);

	if(n->prev != NULL)
		n->prev->next = n->next;
	if(n->next != NULL)
		n->next->prev = n->prev;
	kfree(n);

	return 0;
}

int
ftp_cache_shrink(unsigned long hsh){
	struct ftp_hashlist_node *p;

	DEBUG(" yoyo!\n");
	if(dir_cache.len[hsh] == 0) return -1;
	for(p = dir_cache.hash[hsh]; p->next != NULL; p = p->next);
	ftp_cache_deldir(p);
	dir_cache.len[hsh]--;
	
	return 0;
}

struct ftp_directory*
ftp_cache_add(struct ftp_sb_info* info, char* name){
	unsigned long hsh;
	struct ftp_hashlist_node *p;

	
	hsh = ftp_cache_hash(name);

	while(dir_cache.len[hsh]>=FTP_CACHELEN)
		ftp_cache_shrink(hsh);

	p = (struct ftp_hashlist_node*)kmalloc(sizeof(struct ftp_hashlist_node), GFP_KERNEL);
	if(!p) {
		DEBUG(" kmalloc error!\n");
		goto out;
	}

	memset(p, 0, sizeof(struct ftp_hashlist_node));
	p->directory.valid = 1;
	p->directory.time = CURRENT_TIME;
	p->directory.name = (char*)kmalloc(strlen(name)+1, GFP_KERNEL);
	if(!p->directory.name){
		DEBUG(" kmalloc error!\n");
		goto out1;
	}
	strcpy(p->directory.name,name);

	DEBUG(" loading dir\n");
	if(ftp_loaddir(info, name, &p->directory)<0){
		DEBUG(" couldn't load directory...\n");
		goto out2;
	}
	DEBUG(" dir loaded!\n");
	p->prev = NULL;
	p->next = dir_cache.hash[hsh];
	if(p->next)
		p->next->prev = p;
	dir_cache.hash[hsh] = p;

	dir_cache.len[hsh]++;
	return &p->directory;
out2:	
	kfree(p->directory.name);
out1:
	kfree(p);
out:
	return NULL;
}

struct ftp_directory*
ftp_cache_get(struct ftp_sb_info *info, char* name){
	struct ftp_hashlist_node *p;
	unsigned long hsh;

	DEBUG(" looking for %s\n", name);
	hsh = ftp_cache_hash(name);
	DEBUG(" #21\n");
	for(p = dir_cache.hash[hsh]; p != NULL; p = p->next)
//		DEBUG(" %s\n", p->directory.name);
		if((strcmp(name, p->directory.name) == 0)&&(p->directory.valid)) {
			DEBUG(" %s found in cache!\n", name);
			return &p->directory;
		}
	
	DEBUG(" %s not found in cache. Adding...!\n", name);
	return ftp_cache_add(info, name);
}

int
ftp_cache_empty(){
	int i,j;
	struct ftp_hashlist_node* p;

	DEBUG(" closing cache...\n");
	for(i=0; i<FTP_CACHEHASH; i++)
		for(j=0; j<dir_cache.len[i];j++){
			p = dir_cache.hash[i];
			dir_cache.hash[i] = p->next;
			ftp_cache_deldir(p);
		}
	DEBUG(" OK\n");
	return 0;
}

