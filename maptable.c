/*
 *  XenLoop -- A High Performance Inter-VM Network Loopback
 *
 *  Installation and Usage instructions
 *
 *  Authors:
 *  	Jian Wang - Binghamton University (jianwang@cs.binghamton.edu)
 *  	Kartik Gopalan - Binghamton University (kartik@cs.binghamton.edu)
 *
 *  Copyright (C) 2007-2009 Kartik Gopalan, Jian Wang
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#include "maptable.h"
#include "debug.h"
#include "bififo.h"

extern void send_destroy_chn_msg(u8 *dest_mac);
extern wait_queue_head_t swq;

static DEFINE_SPINLOCK(glock);

ulong  hash(u8 *pmac){
	return (pmac[3] + pmac[4] + pmac[5]) % XENLOOP_HASH_SIZE;
}


int  equal(void *pmac1, void *pmac2)
{
	if (memcmp(pmac1, pmac2, ETH_ALEN) == 0)
		return 1;
	else
		return 0;
};


// hash an IPv4 address
ulong hash_ip(u32 ip) {
	return ip % XENLOOP_HASH_SIZE;
}

// check if two IPv4 address are equal
// returns 1 if equal, 0 otherwise
int equal_ip(u32 ip1, u32 ip2) {
	return ip1 == ip2;
}

// insert into a table
// key is a MAC address, value is a domid
inline void insert_table(HashTable * ht, void * key, u8 domid)
{
	Bucket * b = &ht->table[hash(key)];
	Entry * e;
	ulong flags;

	e = kmem_cache_alloc(ht->entries, GFP_ATOMIC);
	BUG_ON(!e);
	memcpy(e->mac, key, ETH_ALEN);
	e->domid = domid;
	e->timestamp = jiffies;
	e->del_timer = 0;
	e->status = XENLOOP_STATUS_INIT;
	e->listen_flag = 0xff;
	e->bfh = NULL;
	e->retry_count = 0;
	e->ip = 0;

	spin_lock_irqsave(&glock, flags);
	list_add(&e->mapping, &(b->bucket));
	ht->count++;
	spin_unlock_irqrestore(&glock, flags);
}

// insert an Entry at an IPv4 key in a table
// used for IP (not MAC) table
// NOTE: pointer to reference is stored, not copied!
inline void insert_table_ip(HashTable* ht, u32 ip, Entry* e) {
	Bucket * b = &ht->table[hash_ip(ip)];
	// Entry * e;
	ulong flags;

	// e = kmem_cache_alloc(ht->entries, GFP_ATOMIC);
	BUG_ON(!e);

	// memcpy((void*)e, (void*)old_entry, sizeof(Entry));
	e->ip = ip;

	spin_lock_irqsave(&glock, flags);
	// list_add(&e->mapping, &(b->bucket));
	list_add(&e->ip_mapping, &(b->bucket));
	ht->count++;
	spin_unlock_irqrestore(&glock, flags);
}

// remove an entry from the table
// NOTE: deallocates memory for Entries, make sure it isn't references anywhere else
inline void remove_entry(HashTable *ht, Entry *e, struct list_head *x) {
	ulong flags;

	TRACE_ENTRY;
	spin_lock_irqsave(&glock, flags);
	list_del(x);
	ht->count--;
	spin_unlock_irqrestore(&glock, flags);

	// change status first, so suspend doesn't call this while we're disconnecting
	e->status  = XENLOOP_STATUS_INIT;
	if (e->bfh) {
		if(e->listen_flag) {
			bf_destroy(e->bfh);
		} else {
			bf_disconnect(e->bfh);
		}
		e->bfh = NULL;
	}

	if(e->del_timer) {
		del_timer(&e->ack_timer);
	}

	kmem_cache_free(ht->entries, e);
	DPRINTK("Delete Guest: deleted one guest mac =" MAC_FMT " Domid = %d.\n", \
		 MAC_NTOA(e->mac), e->domid);
	TRACE_EXIT;
}

// remove the entry keyed at MAC address 'mac'
inline void remove_entry_mac(HashTable* ht, void* mac) {
	Bucket * b = &ht->table[hash(mac)];

	if(!list_empty(&b->bucket)) {
		struct list_head * x;
		Entry * e;
		list_for_each(x, &(b->bucket)) {
			e = list_entry(x, Entry, mapping);
			if(equal(mac, (u8 *) e->mac)) {
				remove_entry(ht, e, x);
				break;
			}
		}
	}
}

// remove the reference to an entry at an IP
inline void remove_entry_ip(HashTable* ht, u32 ip) {
	ulong flags;
	Bucket * b = &ht->table[hash_ip(ip)];

	if(!list_empty(&b->bucket)) {
		struct list_head * x;
		Entry * e;
		list_for_each(x, &(b->bucket)) {
			e = list_entry(x, Entry, ip_mapping);
			if(ip == e->ip) {
				e->ip = 0;
				spin_lock_irqsave(&glock, flags);
				list_del(x);
				ht->count--;
				spin_unlock_irqrestore(&glock, flags);
				break;
			}
		}
	}
}

// lookup the Entry according to bififo handle pointer 'key'
inline Entry* lookup_bfh(HashTable * ht, void * key)
{
	int i;
	struct list_head * x, * y;
	Entry * e;

	for(i = 0; i < XENLOOP_HASH_SIZE; i++) {
		list_for_each_safe(x, y, &(ht->table[i].bucket)) {
			e = list_entry(x, Entry, mapping);
			if(key ==  e->bfh) {
				return e;
			}
		}
	}
	return NULL;
}

// lookup a MAC address key in the table
inline void * lookup_table(HashTable * ht, void * key)
{
	Entry * d = NULL;
	Bucket * b = &ht->table[hash(key)];

	if(!list_empty(&b->bucket)) {
		struct list_head * x;
		Entry * e;
		list_for_each(x, &(b->bucket)) {
			e = list_entry(x, Entry, mapping);
			if(equal(key, (u8 *) e->mac)) {
				d = e;
				break;
			}
		}
	}
	return d;
}

// lookup an IP address key in the table
inline void * lookup_table_ip(HashTable * ht, u32 ip) {
	Entry * d = NULL;
	Bucket * b = &ht->table[hash_ip(ip)];

	if(!list_empty(&b->bucket)) {
		struct list_head * x;
		Entry * e;
		list_for_each(x, &(b->bucket)) {
			e = list_entry(x, Entry, ip_mapping);
			if(e->ip == ip) {
				d = e;
				break;
			}
		}
	}
	return d;
}

// return 1 if there is a suspended entry in the table, 0 otherwise
inline int has_suspend_entry(HashTable * ht)
{
	int i;
	Entry *e;
	struct list_head *x, *y;
	Bucket * table = ht->table;

	for(i = 0; i < XENLOOP_HASH_SIZE; i++) {
		list_for_each_safe(x, y, &(table[i].bucket)) {
			e = list_entry(x, Entry, mapping);
			if (e->status == XENLOOP_STATUS_SUSPEND)
				return 1;
		}
	}
	return 0;
}

// mark all entries in the table as suspended
inline void mark_suspend(HashTable * ht)
{
	int i;
	Entry *e;
	struct list_head *x, *y;
	Bucket * table = ht->table;
	TRACE_ENTRY;
	for(i = 0; i < XENLOOP_HASH_SIZE; i++) {
		list_for_each_safe(x, y, &(table[i].bucket)) {
			e = list_entry(x, Entry, mapping);
			if (check_descriptor(e->bfh)) {
				BF_SUSPEND_IN(e->bfh) = 1;
				BF_SUSPEND_OUT(e->bfh) = 1;
				bf_notify(e->bfh->port);
			}
			e->status = XENLOOP_STATUS_SUSPEND;
		}
	}
	TRACE_EXIT;
}

// notify all bififos in the table, sends an event to the other side of the bififos
void notify_all_bfs(HashTable * ht)
{
	int i;
	Entry *e;
	struct list_head *x, *y;
	Bucket * table = ht->table;

	TRACE_ENTRY;

	for(i = 0; i < XENLOOP_HASH_SIZE; i++) {
		list_for_each_safe(x, y, &(table[i].bucket)) {
			e = list_entry(x, Entry, ip_mapping);
			if ( check_descriptor(e->bfh) && (xf_size( e->bfh->out ) > 0) )
					bf_notify(e->bfh->port);
		}
	}

	TRACE_EXIT;
}

// check if any entries have timed out (timestamps are too old)
// mark them as suspended if they are
inline void check_timeout(HashTable * ht)
{
	int i, found = 0;
	Entry *e;
	struct list_head *x, *y;
	Bucket * table = ht->table;

	for(i = 0; i < XENLOOP_HASH_SIZE; i++) {
		list_for_each_safe(x, y, &(table[i].bucket)) {
			e = list_entry(x, Entry, mapping);
	 		if ((jiffies - e->timestamp) > (5*DISCOVER_TIMEOUT*HZ)) {
				if (check_descriptor(e->bfh)) {
					BF_SUSPEND_IN(e->bfh) = 1;
					BF_SUSPEND_OUT(e->bfh) = 1;
				}

				DPRINTK("marking entry as suspended\n");
				e->status = XENLOOP_STATUS_SUSPEND;
				found = 1;
			}
		}
	}
	if (found)
		wake_up_interruptible(&swq);
}

// update the timestamps in keys corresponding to the array of MAC address 'mac'
inline void update_table(HashTable * ht, u8 *mac, int mac_count)
{
	int i,j,found = 0;
	Entry *e;
	void *p;
	struct list_head *x, *y;
	Bucket * table = ht->table;


	for(j = 0; j < XENLOOP_HASH_SIZE; j++) {
		list_for_each_safe(x, y, &(table[j].bucket)) {
			e = list_entry(x, Entry, mapping);
			for(i = 0, p = mac;  i < mac_count; i++, p+= ETH_ALEN) {
				if (equal(p, e->mac)) {
					e->timestamp = jiffies;
					found = 1;
					break;
				}

			}

			if (found) {
				found = 0;
				continue;
			}
			if (check_descriptor(e->bfh)) {
				BF_SUSPEND_IN(e->bfh) = 1;
				BF_SUSPEND_OUT(e->bfh) = 1;
			}
			e->status = XENLOOP_STATUS_SUSPEND;
			found = 0;
			wake_up_interruptible(&swq);
		}
	}
}

// initialize a MAC hash table
// NOTE: allocates kmeme cache for Entries
int init_hash_table(HashTable * ht, char * name)
{
	int i;

	ht->count 	= 0;
	ht->entries = kmem_cache_create(name, sizeof(Entry), 0, 0, NULL);

	if(!ht->entries) {
		EPRINTK("hashtable(): slab caches failed.\n");
		return -ENOMEM;
	}

	for(i = 0; i < XENLOOP_HASH_SIZE; i++) {
		INIT_LIST_HEAD(&(ht->table[i].bucket));
	}

	return 0;
}

// initialize an IP hash table
// NOTE: does not allocate any memory, all Entries stored as references
int init_hash_table_ip(HashTable* ht) {
	int i;

	ht->count 	= 0;
	ht->entries = NULL;

	for(i = 0; i < XENLOOP_HASH_SIZE; i++) {
		INIT_LIST_HEAD(&(ht->table[i].bucket));
	}

	return 0;
}

// remove all entries marked suspended
void clean_suspended_entries(HashTable * ht, HashTable* ip_ht)
{
	int i;
	Entry *e;
	struct list_head *x, *y;
	Bucket * table = ht->table;

	DPRINTK("clean suspended entries\n");

	for(i = 0; i < XENLOOP_HASH_SIZE; i++) {
		list_for_each_safe(x, y, &(table[i].bucket)) {
			e = list_entry(x, Entry, mapping);
			if (e->status == XENLOOP_STATUS_SUSPEND) {
				if(e->ip) {
					remove_entry_ip(ip_ht, e->ip);
				}

				remove_entry(ht, e, x);
			}
		}
	}
}

// remove all entries in the table
void clean_table(HashTable * ht)
{
	int i;
	Entry *e;
	struct list_head *x, *y;
	Bucket * table = ht->table;

	DPRINTK("clean table\n");

	for(i = 0; i < XENLOOP_HASH_SIZE; i++) {
		list_for_each_safe(x, y, &(table[i].bucket)) {
			e = list_entry(x, Entry, mapping);
			remove_entry(ht, e, x);
		}
	}

    kmem_cache_destroy(ht->entries);
	//BUG_ON(kmem_cache_destroy(ht->entries));
}
