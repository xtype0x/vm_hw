/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include <stdlib.h>
#include "exec-all.h"
#include "tcg-op.h"
#include "helper.h"
#define GEN_HELPER 1 
#include "helper.h"
#include "optimization.h"

extern uint8_t *optimization_ret_addr;
/*
 * Shadow Stack
 
    uint64_t *shack;                                                    \
    uint64_t *shack_top;                                                \
    uint64_t *shack_end;                                                \
    void *shadow_hash_list;                                             \
 */
struct shack_hash_entry {
	target_ulong guest_eip;
	unsigned long* host_eip;
};
typedef struct shack_hash_entry shack_hash_entry;
shack_hash_entry *shadow_hash_list;

static inline void shack_init(CPUState *env){
	int i;
	env->shack = (uint64_t *)malloc(SHACK_SIZE * sizeof(uint64_t));
	env->shack_top = env->shack;
	env->shack_end = env->shack + SHACK_SIZE;
	shadow_hash_list = (shack_hash_entry *)malloc(SHACK_SIZE * sizeof(shack_hash_entry));
	env->shadow_hash_list = shadow_hash_list;

	/*Init hash*/
	for(i=0; i<SHACK_SIZE; i++) {
		shadow_hash_list[i].guest_eip = 0;
		shadow_hash_list[i].host_eip = (unsigned long*)optimization_ret_addr;
	}
} 

/*
 * shack_set_shadow()
 *  Insert a guest eip to host eip pair if it is not yet created.
 */
void shack_set_shadow(CPUState *env, target_ulong guest_eip, unsigned long *host_eip){	
	int table_index = guest_eip & (SHACK_SIZE-1);
	shadow_hash_list[table_index].guest_eip = guest_eip;
	shadow_hash_list[table_index].host_eip = host_eip;
}

/*
 * helper_shack_flush()
 *  Reset shadow stack.
 */
void helper_shack_flush(CPUState *env){
	env->shack_top = env->shack + 1;
}

/*
 * push_shack()
 *  Push next guest eip into shadow stack.
 */
void push_shack(CPUState *env, TCGv_ptr cpu_env, target_ulong next_eip) {
	TCGv_ptr temp_shack_top = tcg_temp_new_ptr();
	TCGv_ptr temp_shack_end = tcg_temp_new_ptr();
	TCGv_ptr temp_entry_ptr = tcg_temp_new_ptr();
	TCGv temp_next_eip = tcg_temp_local_new_i32();
	// int flush_label = gen_new_label();
	shack_hash_entry *entry;

	//Load the entry. Entry is the constant for each next_eip
	int table_index = next_eip & (SHACK_SIZE-1);
	entry = &shadow_hash_list[table_index];
	
	// load to temp
	tcg_gen_ld_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top));
	tcg_gen_ld_ptr(temp_shack_end, cpu_env, offsetof(CPUState, shack_end));
	tcg_gen_movi_i32(temp_next_eip, next_eip);
	tcg_gen_movi_i32(temp_entry_ptr ,entry);

	//- branch to flush
	// tcg_gen_brcond_ptr(TCG_COND_EQ, temp_shack_top, temp_shack_end, flush_label);

	// push to stack
	tcg_gen_st_ptr(temp_entry_ptr, temp_shack_top, 0);
	tcg_gen_addi_ptr(temp_shack_top, temp_shack_top, sizeof(uint64_t));
	tcg_gen_st_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top));

	// gen_set_label(flush_label);
	// printf("");//do nothing
	// // flush stack
	// helper_shack_flush(env);
	// tcg_gen_mov_tl(temp_shack_top, tcg_const_tl((int32_t)(env->shack + 1)));
}

/*
 * pop_shack()
 *  Pop next host eip from shadow stack.
 */
// #define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *) 0)->MEMBER)
void pop_shack(TCGv_ptr cpu_env, TCGv next_eip){
	//----------------------TCG operations 
	TCGv_ptr temp_shack_top = tcg_temp_new_ptr();
	TCGv_ptr temp_shack_empty = tcg_temp_new_ptr();
	TCGv_ptr temp_shack_top_next = tcg_temp_new_ptr();
	TCGv_ptr temp_entry_ptr = tcg_temp_new_ptr();
	TCGv_ptr temp_host_eip = tcg_temp_new_ptr();
	TCGv temp_guest_eip = tcg_temp_local_new_i32();
	TCGv temp_next_eip = tcg_temp_local_new_i32();
	int eip_match_label = gen_new_label(); // create label
	int empty_label = gen_new_label(); // create label
	
	//load next_eip into temp_next_eip
	tcg_gen_mov_i32(temp_next_eip, next_eip);
	// load shack_top ptr into temp_shack_top	
	tcg_gen_ld_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top));
	tcg_gen_ld_ptr(temp_shack_empty, cpu_env, offsetof(CPUState, shack));

	//check if stack is empty
	tcg_gen_brcond_ptr(TCG_COND_EQ, temp_shack_top, temp_shack_empty, empty_label);

		// get (shack_top - 8)
		tcg_gen_ld_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top));
		tcg_gen_subi_tl(temp_shack_top_next, temp_shack_top, sizeof(uint64_t));
		// get entry
		tcg_gen_ld_ptr(temp_entry_ptr, temp_shack_top_next, 0);
		// load entry.guest_eip
		tcg_gen_ld_ptr(temp_guest_eip, temp_entry_ptr, offsetof(shack_hash_entry, guest_eip));

		// branch to label
		tcg_gen_brcond_ptr(TCG_COND_NE, temp_next_eip, temp_guest_eip, eip_match_label);
		//reload all temps.
			tcg_gen_ld_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top));
			tcg_gen_subi_tl(temp_shack_top_next, temp_shack_top, sizeof(uint64_t));
			tcg_gen_ld_ptr(temp_entry_ptr, temp_shack_top_next, 0);
			// load entry.host_eip into temp_host_eip
			tcg_gen_ld_ptr(temp_host_eip, temp_entry_ptr, offsetof(shack_hash_entry, host_eip));
			// update shack_top
			tcg_gen_st_ptr(temp_shack_top_next, cpu_env, offsetof(CPUState, shack_top)); 
			
			// jump
			*gen_opc_ptr++ = INDEX_op_jmp;
			*gen_opparam_ptr++ = temp_host_eip;
		
		// set label here
		gen_set_label(eip_match_label);
	
	// empty stack do nothing
	gen_set_label(empty_label);
}

/*
 * Indirect Branch Target Cache
 */
__thread int update_ibtc;
struct ibtc_table *lookup_table;
int ibtc_table_index;
target_ulong tmp_pc;
/*
 * helper_lookup_ibtc()
 *  Look up IBTC. Return next host eip if cache hit or
 *  back-to-dispatcher stub address if cache miss.
 */
void *helper_lookup_ibtc(target_ulong guest_eip){
	//Use guest_eip to look up the table
	ibtc_table_index = guest_eip & IBTC_CACHE_MASK;
	
	if(lookup_table->htable[ibtc_table_index].guest_eip == guest_eip) {
		return lookup_table->htable[ibtc_table_index].tb->tc_ptr;
	}
		
	//if no found
	update_ibtc = 1;
	tmp_pc = guest_eip;

	return optimization_ret_addr;
}

/*
 * update_ibtc_entry()
 *  Populate eip and tb pair in IBTC entry.
 */
void update_ibtc_entry(TranslationBlock *tb){
	update_ibtc = 0;
	lookup_table->htable[ibtc_table_index].guest_eip = tmp_pc;
	lookup_table->htable[ibtc_table_index].tb = tb;
}
/*
 * ibtc_init()
 *  Create and initialize indirect branch target cache.
 */
static inline void ibtc_init(CPUState *env){
	/*Initiate ibtc_table*/
	lookup_table = (struct ibtc_table *)malloc(sizeof(struct ibtc_table));
	ibtc_table_index = 0;
	update_ibtc = 0;
}

/*
 * init_optimizations()
 *  Initialize optimization subsystem.
 */
int init_optimizations(CPUState *env){
    shack_init(env);
    ibtc_init(env);

    return 0;
}

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */  
