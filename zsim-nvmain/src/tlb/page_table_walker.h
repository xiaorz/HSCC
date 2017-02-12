/*
 * 2015-xxx by YuJie Chen
 * email:    YuJieChen_hust@163.com
 * function: extend zsim-nvmain with some other simulation,such as tlb,page table,page table,etc.  
 */
#ifndef PAGE_TABLE_WALKER_H
#define PAGE_TABLE_WALKER_H

#include <map>
#include <fstream>
#include "locks.h"
#include "g_std/g_string.h"
#include "memory_hierarchy.h"
#include "page-table/comm_page_table_op.h"
#include "common/global_const.h"
#include "page-table/page_table.h"

#include "MMU/memory_management.h"
#include "tlb/common_func.h"
#include "tlb/hot_monitor_tlb.h"
#include "DRAM-buffer/DRAM_buffer_block.h"
#include "tlb/tlb_entry.h"

#include "src/nvmain_mem_ctrl.h"
#include "include/NVMainRequest.h"
#include "core.h"
#include "ooo_core.h"

template <class T>
class PageTableWalker: public BasePageTableWalker
{
	public:
		//access memory
		PageTableWalker(const g_string& name , PagingStyle style): pg_walker_name(name),procIdx((uint32_t)(-1))
		{
			mode = style;
			period = 0;
			dirty_evict = 0;
			total_evict = 0;
			allocated_page = 0;
			mmap_cached = 0;
			tlb_shootdown_overhead = 0;
			hscc_tlb_shootdown = 0;
			pcm_map_overhead = 0;
			hscc_map_overhead = 0;
			tlb_miss_exclude_shootdown = 0;
			tlb_miss_overhead = 0;
			futex_init(&walker_lock);
		}
		~PageTableWalker(){}
		/*------tlb hierarchy related-------*/
		//bool add_child(const char* child_name ,  BaseTlb* tlb);
		/*------simulation timing and state related----*/
		uint64_t access( MemReq& req)
		{

			assert(paging);
			period++;
			Address addr = PAGE_FAULT_SIG;
			Address init_cycle = req.cycle;
			addr = paging->access(req);

			tlb_miss_exclude_shootdown += (req.cycle - init_cycle);
			//page fault
			if( addr == PAGE_FAULT_SIG )	
			{
				addr = do_page_fault(req , PCM_PAGE_FAULT);
			}
			tlb_miss_overhead += (req.cycle - init_cycle);
			//suppose page table walking time when tlb miss is 20 cycles
			return addr;	//find address
		}

		void write_through( MemReq& req)
		{
			assert(paging);
			paging->access(req);
		}

		BasePaging* GetPaging()
		{ return paging;}
		void SetPaging( uint32_t proc_id , BasePaging* copied_paging)
		{
			futex_lock(&walker_lock);
			procIdx = proc_id;
			paging = copied_paging;
			futex_unlock(&walker_lock);
		}

		void convert_to_dirty( Address block_id)
		{
			zinfo->dram_manager->convert_to_dirty( procIdx , block_id );
		}
		const char* getName()
		{  return pg_walker_name.c_str(); }

		void calculate_stats()
		{
			info("%s evict time:%lu \t dirty evict time: %lu \n",getName(),total_evict,dirty_evict);
			info("%s allocated pages:%lu \n", getName(),allocated_page);
			info("%s TLB shootdown overhead:%llu \n", getName(), tlb_shootdown_overhead);
			info("%s HSCC TLB shootdown overhead:%llu \n", getName(), hscc_tlb_shootdown);
			info("%s PCM page mapping overhead:%llu \n", getName(), pcm_map_overhead);
			info("%s DRAM page mapping overhead:%llu \n", getName(), hscc_map_overhead);
			info("%s TLB miss overhead(exclude TLB shootdown and page fault): %llu", getName(),tlb_miss_exclude_shootdown);
			info("%s TLB miss overhead (include TLB shootdown and page fault): %llu",getName(), tlb_miss_overhead);
		}
		
		Address do_page_fault(MemReq& req, PAGE_FAULT fault_type)
		{
		    //allocate one page from Zone_Normal area
		    debug_printf("page falut, allocate free page through buddy allocator");
		    Page* page = NULL;
			if( zinfo->buddy_allocator)
			{
				page = zinfo->buddy_allocator->allocate_pages(0);
				if(page)
				{
					//TLB shootdown
					Address vpn = req.lineAddr>>(zinfo->page_shift);
					BaseTlb* tmp_tlb = NULL;
					T* entry = NULL;
					for( uint64_t i = 0; i<zinfo->numCores; i++)
					{
						tmp_tlb = zinfo->cores[i]->getInsTlb();
						union
						{	
							CommonTlb<T>* com_tlb;
							HotMonitorTlb<T>* hot_tlb;
						};
						if( zinfo->tlb_type == COMMONTLB )
						{
							com_tlb = dynamic_cast<CommonTlb<T>* >(tmp_tlb); 
							entry = com_tlb->look_up(vpn);
						}
						else if( zinfo->tlb_type == HOTTLB)
						{
							hot_tlb = dynamic_cast<HotMonitorTlb<T>* >(tmp_tlb); 
							entry = hot_tlb->look_up(vpn);
						}
						//instruction TLB IPI
						if( entry )
						{
							entry->set_invalid();
							tlb_shootdown_overhead += zinfo-> tlb_hit_lat; 
							req.cycle += zinfo->tlb_hit_lat;
							entry = NULL;
						}
						tmp_tlb = zinfo->cores[i]->getDataTlb();
						if( zinfo->tlb_type == COMMONTLB)
						{
							com_tlb = dynamic_cast<CommonTlb<T>* >(tmp_tlb); 
							entry = com_tlb->look_up(vpn);
						}
						else if( zinfo->tlb_type == HOTTLB)
						{
							hot_tlb = dynamic_cast<HotMonitorTlb<T>* >(tmp_tlb); 
							entry = hot_tlb->look_up(vpn);
						}
						if(entry )
						{
							entry->set_invalid();
							tlb_shootdown_overhead += zinfo-> tlb_hit_lat; 
							req.cycle += zinfo->tlb_hit_lat;
							entry = NULL;
						}
					}
					//*********TLB shoot down ended
					if( zinfo->enable_shared_memory)
					{
						if( !map_shared_region(req, page) )
						{
							Address overhead = paging->map_page_table( req.lineAddr,(void*)page);
							pcm_map_overhead += overhead;
							req.cycle += overhead;
						}
					}
					else
					{
						Address overhead = paging->map_page_table( req.lineAddr,(void*)page);
						pcm_map_overhead += overhead;
						req.cycle += overhead;
					}
					allocated_page++;
					return page->pageNo;
				}
			}
			//update page table
			return (req.lineAddr>>zinfo->page_shift);
		}

		
		bool inline map_shared_region( MemReq& req , void* page)
		{
			Address vaddr = req.lineAddr;
			//std::cout<<"find out shared region"<<std::endl;
			if( !zinfo->shared_region[procIdx].empty())
			{
				int vm_size = zinfo->shared_region[procIdx].size();
				//std::cout<<"mmap_cached:"<<std::dec<<mmap_cached
				//	<<" vm size:"<<std::dec<<vm_size<<std::endl;
				Address vm_start = zinfo->shared_region[procIdx][mmap_cached].start;
				Address vm_end = zinfo->shared_region[procIdx][mmap_cached].end;
				Address vpn = vaddr>>(zinfo->page_shift);
				//belong to the shared memory region
				//examine whether in mmap_cached region (examine mmap_cached firstly)
				if( find_shared_vm(vaddr,
						zinfo->shared_region[procIdx][mmap_cached]) )
				{
					Address overhead = map_all_shared_memory( vaddr, (void*)page);
					req.cycle += overhead;
					pcm_map_overhead += overhead;
					return true;
				}
				//after mmap_cached
				else if( vpn > vm_end && mmap_cached < vm_size-1 )
				{
					mmap_cached++;
					for( ;mmap_cached < vm_size; mmap_cached++)
					{
						if( find_shared_vm(vaddr, 
							zinfo->shared_region[procIdx][mmap_cached]))
						{
							Address overhead = map_all_shared_memory(vaddr, (void*)page);
							req.cycle += overhead;
							pcm_map_overhead += overhead;
							return true;
						}
					}
					mmap_cached--;
				}
				//before mmap_cached
				else if( vpn < vm_start && mmap_cached > 0 )
				{
					mmap_cached--;
					for( ;mmap_cached >= 0; mmap_cached--)
					{
						if( find_shared_vm(vaddr, 
							zinfo->shared_region[procIdx][mmap_cached]))
						{
							Address overhead = map_all_shared_memory(vaddr, (void*)page);
							req.cycle += overhead;
							pcm_map_overhead += overhead;
							return true;
						}
					}
					mmap_cached++;
				}
			}
			return false;
		}

		bool inline find_shared_vm(Address vaddr, Section vm_sec)
		{
			Address vpn = vaddr >>(zinfo->page_shift);
			if( vpn >= vm_sec.start && vpn < vm_sec.end )
				return true;
			return false;
		}

		int inline map_all_shared_memory( Address va, void* page)
		{
			int latency = 0;
			for( uint32_t i=0; i<zinfo->numProcs; i++)
			{
				assert(zinfo->paging_array[i]);
				latency += zinfo->paging_array[i]->map_page_table(va,page);
			}
			return latency;
		}

		DRAMBufferBlock* allocate_page( )
		{
			if( zinfo->dram_manager->should_reclaim() )
			{
				zinfo->dram_manager->evict( zinfo->dram_evict_policy);
			}
			//std::cout<<"allocate page table"<<std::endl;
			return (zinfo->dram_manager)->allocate_one_page( procIdx);
		}

		void reset_tlb( T* tlb_entry)
		{
			tlb_entry->set_in_dram(false);
			tlb_entry->clear_counter();
		}
		

		Address do_dram_page_fault(MemReq& req, Address vpn ,uint32_t coreId, PAGE_FAULT fault_type , T* entry , bool is_itlb , bool &evict)
		{
			debug_printf("fetch pcm page %d to DRAM",(req.lineAddr>>zinfo->page_shift));
			//allocate dram buffer block
			DRAMBufferBlock* dram_block = allocate_page();
			if( dram_block)
			{
				Address dram_addr = block_id_to_addr( dram_block->block_id);
				//evict dram_block if it's dirty
				if( dram_block->is_occupied())
				{
					total_evict++;
					Address origin_ppn = dram_block->get_src_addr();
					if( dram_block->is_dirty())
					{
						evict = true;
						dirty_evict++;
						Address dst_addr = origin_ppn<<(zinfo->page_shift);
						//write back
						if( NVMainMemory::fetcher)
						{
							NVM::NVMainRequest* nvmain_req = new NVM::NVMainRequest();
							//from nvm to dram
							//is nvm address
							nvmain_req->address.SetPhysicalAddress(dram_addr,true);
							//buffer address
							nvmain_req->address.SetDestAddress(dst_addr, false);
							nvmain_req->burstCount = zinfo->block_size;
							nvmain_req->type = NVM::FETCH; 
							(NVMainMemory::fetcher)->IssueCommand( nvmain_req );
						}
						else 
						{
							req.cycle +=200;
						}
					}
					//remove relations between tlb and invalidate dram
					Address vaddr = dram_block->get_vaddr();	
					T* tlb_entry = NULL;
					HotMonitorTlb<T>* recover_tlb = NULL;
					//TLB shootdown1: shootdown evicted pages
					//TLB shootdown2: related to installed pages
					Address vpage_installed = entry->v_page_no;
					for( uint32_t i=0; i<zinfo->numCores; i++)
					{
						recover_tlb = dynamic_cast<HotMonitorTlb<T>* >
									  (zinfo->cores[i]->getInsTlb());;
						tlb_entry = recover_tlb->look_up( vaddr );
						//instruction TLB shootdown(for PCM pages )
						//assume IPI is equal to TLB hit cycle
						if( tlb_entry)
						{
							reset_tlb( tlb_entry);
							hscc_tlb_shootdown += zinfo->tlb_hit_lat;
							req.cycle += zinfo->tlb_hit_lat;	//IPI latency 
							tlb_entry = NULL;
						}
						//instruction TLB shootdown(for DRAM pages)
						tlb_entry = recover_tlb->look_up(vpage_installed);
						if( tlb_entry)
						{
							reset_tlb(tlb_entry);
							hscc_tlb_shootdown += zinfo->tlb_hit_lat;
							req.cycle += zinfo->tlb_hit_lat;  //IPI latency
							tlb_entry = NULL;
						}
						//data TLB shootdown( for PCM pages)
						recover_tlb = dynamic_cast<HotMonitorTlb<T>* >
									  (zinfo->cores[i]->getDataTlb());;
						tlb_entry = recover_tlb->look_up( vaddr );
						if( tlb_entry )
						{
							reset_tlb(tlb_entry);
							hscc_tlb_shootdown += zinfo->tlb_hit_lat;
							req.cycle += zinfo->tlb_hit_lat; //IPI latency
							tlb_entry = NULL;
						}
						//data TLB shootdown(for DRAM pages)
						tlb_entry = recover_tlb->look_up(vpage_installed);
						if( tlb_entry )
						{
							reset_tlb(tlb_entry);
							hscc_tlb_shootdown += zinfo->tlb_hit_lat;
							req.cycle += zinfo->tlb_hit_lat; //IPI latency
							tlb_entry = NULL;
						}
					}
					
					Page* page_ptr = zinfo->memory_node->get_page_ptr(origin_ppn); 
					Address overhead = paging->map_page_table((vaddr<<zinfo->page_shift),(void*)page_ptr,false);
					req.cycle += overhead;
					hscc_map_overhead += overhead;
					dram_block->invalidate();
				}
				//call memory controller interface to cache page
				 if( NVMainMemory::fetcher)
				{
					NVM::NVMainRequest* nvm_req = new NVM::NVMainRequest();
					nvm_req->address.SetPhysicalAddress(req.lineAddr ,false );
					nvm_req->address.SetDestAddress(dram_addr , true);
					nvm_req->burstCount = zinfo->block_size;
					NVMainMemory::fetcher->IssueCommand( nvm_req );
				}
				else	//add fix latency express data buffering 
				{
					req.cycle += 200;
				}

				dram_block->validate(req.lineAddr>>(zinfo->block_shift));
				HotMonitorTlb<T>* tlb = NULL;		

				if( is_itlb )
					tlb = dynamic_cast<HotMonitorTlb<T>* >(zinfo->cores[coreId]->getInsTlb());
				else
					tlb = dynamic_cast<HotMonitorTlb<T>* >(zinfo->cores[coreId]->getDataTlb());
				
				tlb->remap_to_dram((dram_addr>>(zinfo->block_shift)) , entry);

			    dram_block->set_src_addr( entry->p_page_no );
				dram_block->set_vaddr( entry->v_page_no);
				debug_printf("after remap , vpn:%llx , ppn:%llx",entry->v_page_no , entry->p_page_no);
				//update extended page table
				Address overhead= paging->map_page_table((vpn<<zinfo->page_shift),(void*)dram_block,true);
				req.cycle += overhead;
				hscc_map_overhead += overhead;	
				return dram_addr;
			}
			return INVALID_PAGE_ADDR;
		}
public:
		PagingStyle mode;
		g_string pg_walker_name;
	    BasePaging* paging;
		uint64_t period;
		unsigned long long tlb_shootdown_overhead;
		unsigned long long hscc_tlb_shootdown;
		unsigned long long pcm_map_overhead;
		unsigned long long hscc_map_overhead;

		unsigned long long tlb_miss_exclude_shootdown;
		unsigned long long tlb_miss_overhead;
	private:
		uint32_t procIdx;
		int mmap_cached;
		Address allocated_page;
		Address total_evict;
		Address dirty_evict;
		lock_t walker_lock;
		//std::list<NVM::NVMainRequest*> failed_cache_request;
};
#endif

