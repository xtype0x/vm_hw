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
struct shack_hash_entry {
	target_ulong guest_eip;
	unsigned long* host_eip;
};
typedef struct shack_hash_entry shack_hash_entry;

/*
 * Shadow Stack
 */
list_t *shadow_hash_list;

static inline void shack_init(CPUState *env){
  env->shack = (uint64_t*) malloc(SHACK_SIZE * sizeof(uint64_t));
  env->shack_top = env->shack;
  env->shack_end = env->shack + SHACK_SIZE;
  env->shadow_ret_addr = (unsigned long *) malloc(SHACK_SIZE * sizeof(unsigned long));
  env->shadow_ret_count = 0;
  env->shadow_hash_list = (void *)malloc(SHACK_SIZE * sizeof(shack_hash_entry));
  int i;
  for(i=0; i<SHACK_SIZE; i++) {
	((shack_hash_entry*)env->shadow_hash_list)[i].guest_eip = 0;
	((shack_hash_entry*)env->shadow_hash_list)[i].host_eip = (unsigned long*)optimization_ret_addr;
  }
}

/*
 * shack_set_shadow()
 *  Insert a guest eip to host eip pair if it is not yet created.
 */
void shack_set_shadow(CPUState *env, target_ulong guest_eip, unsigned long *host_eip){
  list_t *it = ((list_t *)env->shadow_hash_list) + tb_jmp_cache_hash_func(guest_eip);
  it = it->next;
  while(it){
  	struct shadow_pair *sp = (struct shadow_pair *)it;
  	if(sp->guest_eip == guest_eip){
  	  *sp->shadow_slot = (uintptr_t)host_eip;
  	  return;
  	}
  	it = it->next;
  }
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
void push_shack(CPUState *env, TCGv_ptr cpu_env, target_ulong next_eip){
  TCGv_ptr temp_shack_top = tcg_temp_new_ptr();
  TCGv_ptr temp_shack_end = tcg_temp_new_ptr();
  TCGv temp_next_eip = tcg_temp_new();
  int label = gen_new_label();

  tcg_gen_ld_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top));
  tcg_gen_ld_ptr(temp_shack_end, cpu_env, offsetof(CPUState, shack_end));
  tcg_gen_addi_ptr(temp_shack_top, temp_shack_top, sizeof(uint64_t));
  tcg_gen_movi_tl(temp_next_eip,next_eip);

  //branch
  tcg_gen_brcond_tl(TCG_COND_NE, temp_shack_top, temp_shack_end, label);

  //flush shadow stack
  tcg_gen_mov_tl(temp_shack_top, tcg_const_tl((int32_t)env->shack + 1));

  gen_set_label(label);

  //push next eip to stack
  tcg_gen_st_tl(temp_next_eip, temp_shack_top, 0);
  //push address to stack
  tcg_gen_st_tl(tcg_const_tl((int32_t)(env->shadow_ret_addr + env->shadow_ret_count)), temp_shack_top, sizeof(target_ulong));

  tcg_gen_st_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top));

  tcg_temp_free_ptr(temp_shack_top);
  tcg_temp_free_ptr(temp_shack_end);

  env->shadow_ret_count++;
}

/*
 * pop_shack()
 *  Pop next host eip from shadow stack.
 */
void pop_shack(TCGv_ptr cpu_env, TCGv next_eip){
  TCGv_ptr temp_shack_top = tcg_temp_new_ptr();
  TCGv temp_next_eip = tcg_temp_new();
  TCGv guest_eip = tcg_temp_new();
  TCGv_ptr host_eip_addr = tcg_temp_new_ptr();
  TCGv_ptr host_eip = tcg_temp_new_ptr();
  int label = gen_new_label();

  //load to temp
  tcg_gen_ld_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top));
  tcg_gen_ld_tl(guest_eip, temp_shack_top, 0);
  tcg_gen_ld_tl(host_eip_addr, temp_shack_top, sizeof(target_ulong));
  tcg_gen_ld_tl(host_eip, host_eip_addr, 0);
  tcg_gen_movi_tl(temp_next_eip, next_eip);

  //branch
  tcg_gen_brcond_tl(TCG_COND_NE, next_eip, guest_eip, label);

  //pop
  tcg_gen_subi_tl(temp_shack_top, temp_shack_top, 2 * sizeof(target_ulong));
  tcg_gen_st_ptr(temp_shack_top, cpu_env, offsetof(CPUState, shack_top));

  tcg_gen_brcond_tl(TCG_COND_EQ, host_eip, tcg_const_tl(0), label);

  *gen_opc_ptr++ = INDEX_op_jmp;
  *gen_opparam_ptr++ = (host_eip);

  gen_set_label(label);

  tcg_gen_movi_tl(host_eip, optimization_ret_addr);
  *gen_opc_ptr++ = INDEX_op_jmp;
  *gen_opparam_ptr++ = (host_eip);  
  //free 
  tcg_temp_free_ptr(temp_shack_top);
  tcg_temp_free(guest_eip);
  tcg_temp_free(host_eip);
  tcg_temp_free(host_eip_addr);
}

/*
 * Indirect Branch Target Cache
 */
__thread int update_ibtc;
struct ibtc_table *itable;

/*
 * helper_lookup_ibtc()
 *  Look up IBTC. Return next host eip if cache hit or
 *  back-to-dispatcher stub address if cache miss.
 */
void *helper_lookup_ibtc(target_ulong guest_eip){
  int ind = guest_eip & IBTC_CACHE_MASK;
  
  if(itable->htable[ind].guest_eip == guest_eip){
    return itable->htable[ind].tb->tc_ptr;
  }
  return optimization_ret_addr;
}

/*
 * update_ibtc_entry()
 *  Populate eip and tb pair in IBTC entry.
 */
void update_ibtc_entry(TranslationBlock *tb){
  int ind = tb->pc & IBTC_CACHE_MASK;
  itable->htable[ind].guest_eip = tb->pc;
  itable->htable[ind].tb = tb;
}

/*
 * ibtc_init()
 *  Create and initialize indirect branch target cache.
 */
static inline void ibtc_init(CPUState *env){
  itable = (struct ibtc_table *)malloc(sizeof(struct ibtc_table));
  memset(itable,0,sizeof(struct ibtc_table));
}

/*
 * init_optimizations()
 *  Initialize optimization subsystem.
 */
int init_optimizations(CPUState *env)
{
    shack_init(env);
    ibtc_init(env);

    return 0;
}

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
