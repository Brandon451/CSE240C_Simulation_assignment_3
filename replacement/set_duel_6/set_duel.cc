//Set duelling implemented for Hawkeye and SHiP++
//Brandon Saldanha

#include "cache.h"

#include <map>

#define NUM_CORE 1
#define LLC_SETS 2048
#define LLC_WAYS 16

#define SAT_INC(x, max) (x < max) ? x + 1 : x
#define SAT_DEC(x) (x > 0) ? x - 1 : x
#define TRUE 1
#define FALSE 0

#define RRIP_OVERRIDE_PERC 0

// 3-bit RRIP counters or all lines
#define maxRRPV_hawkeye 7
uint32_t rrpv_hawkeye[LLC_SETS][LLC_WAYS];

// The base policy is SRRIP. SHIP needs the following on a per-line basis
#define maxRRPV_shippp 3

#define maxRRPV 3
uint32_t rrpv[LLC_SETS][LLC_WAYS];

uint32_t line_rrpv[LLC_SETS][LLC_WAYS];
uint32_t is_prefetch[LLC_SETS][LLC_WAYS];
uint32_t fill_core[LLC_SETS][LLC_WAYS];

// Per-set timers; we only use 64 of these
// Budget = 64 sets * 1 timer per set * 10 bits per timer = 80 bytes
#define TIMER_SIZE 1024
uint64_t perset_mytimer[LLC_SETS];

// These two are only for sampled sets (we use 64 sets)
#define NUM_LEADER_SETS 64

// Signatures for sampled sets; we only use 64 of these
// Budget = 64 sets * 16 ways * 12-bit signature per line = 1.5B
uint64_t signatures[LLC_SETS][LLC_WAYS];
bool prefetched[LLC_SETS][LLC_WAYS];

uint32_t ship_sample[LLC_SETS];
uint32_t line_reuse[LLC_SETS][LLC_WAYS];
uint64_t line_sig[LLC_SETS][LLC_WAYS];

uint32_t hawkeye_sample_dip[LLC_SETS];
uint32_t ship_sample_dip[LLC_SETS];

// Hawkeye Predictors for demand and prefetch requests
// Predictor with 2K entries and 5-bit counter per entry
// Budget = 2048*5/8 bytes = 1.2KB
#define MAX_SHCT 31
#define SHCT_SIZE_BITS 12
#define SHCT_SIZE_hawkeye (1 << SHCT_SIZE_BITS)
#include "../hawkeye/hawkeye_predictor.h"
HAWKEYE_PC_PREDICTOR* demand_predictor;   // Predictor
HAWKEYE_PC_PREDICTOR* prefetch_predictor; // Predictor

// SHCT. Signature History Counter Table
// per-core 16K entry. 14-bit signature = 16k entry. 3-bit per entry
#define maxSHCTR 7
#define SHCT_bits 14
#define SHCT_SIZE_shippp (1 << SHCT_bits)
uint32_t SHCT[NUM_CORE][SHCT_SIZE_shippp];

#define OPTGEN_VECTOR_SIZE 128
#include "../hawkeye/optgen.h"
OPTgen perset_optgen[LLC_SETS]; // per-set occupancy vectors; we only use 64 of these

// Statistics
// uint64_t insertion_distrib[NUM_TYPES][maxRRPV_shippp + 1];
uint64_t insertion_distrib[NUM_TYPES][maxRRPV + 1];
uint64_t total_prefetch_downgrades;

#include <math.h>
#define bitmask(l) (((l) == 64) ? (unsigned long long)(-1LL) : ((1LL << (l)) - 1LL))
#define bits(x, i, l) (((x) >> (i)) & bitmask(l))
// Sample 64 sets per core
#define SAMPLED_SET(set) (bits(set, 0, 6) == bits(set, ((unsigned long long)log2(LLC_SETS) - 6), 6))

// Sampler to track 8x cache history for sampled sets
// 2800 entris * 4 bytes per entry = 11.2KB
#define SAMPLED_CACHE_SIZE 1400//2800
#define SAMPLER_WAYS 8
#define SAMPLER_SETS SAMPLED_CACHE_SIZE / SAMPLER_WAYS
vector<map<uint64_t, ADDR_INFO>> addr_history; // Sampler

#define maxCNTRval 0x3FF

uint32_t select_sat_counter = maxCNTRval/2;
uint32_t hysteresis_check = 0; 

uint32_t shippp_misses = 0;
uint32_t hawkeye_misses = 0;

// initialize replacement state hawkeye
void initialize_replacement_hawkeye()
{
    for (int i = 0; i < LLC_SETS; i++) {
        for (int j = 0; j < LLC_WAYS; j++) {
            //rrpv[i][j] = maxRRPV;
            rrpv_hawkeye[i][j] = maxRRPV_hawkeye;
            signatures[i][j] = 0;
            prefetched[i][j] = false;
        }
        perset_mytimer[i] = 0;
        perset_optgen[i].init(LLC_WAYS - 2);
    }

    for (unsigned int i = 0; i < LLC_SETS; i++) {
        //cout << ((~i>>6) & 0x1F) << " " << (i & 0x1F) << endl;
        if (((i>>6) & 0x1F) == (i & 0x1F)) {
            //cout << "Hawkeye " << i << endl;
            hawkeye_sample[i] = 1;
        }
        //if (((~i>>6) & 0x1F) == (i & 0x1F))
            //cout << "Ship++ " << i << endl;
    }

    addr_history.resize(SAMPLER_SETS);
    for (int i = 0; i < SAMPLER_SETS; i++)
        addr_history[i].clear();

    demand_predictor = new HAWKEYE_PC_PREDICTOR();
    prefetch_predictor = new HAWKEYE_PC_PREDICTOR();

    cout << "Initialize Hawkeye state" << endl;
}

// initialize replacement state
void initialize_replacement_shippp(uint32_t NUM_SET)
{
  //int LLC_SETS = (get_config_number() <= 2) ? 2048 : LLC_SETS;

    cout << "Initialize SRRIP state" << endl;

    for (int i = 0; i < LLC_SETS; i++) {
        for (int j = 0; j < LLC_WAYS; j++) {
            line_rrpv[i][j] = maxRRPV_shippp;
            //rrpv[i][j] = maxRRPV;
            line_reuse[i][j] = FALSE;
            is_prefetch[i][j] = FALSE;
            line_sig[i][j] = 0;
        }
  }

    for (int i = 0; i < NUM_CORE; i++) {
        for (int j = 0; j < SHCT_SIZE_shippp; j++) {
            SHCT[i][j] = 1; // Assume weakly re-use start
        }
    }

    int leaders = 0;

    for (unsigned int i = 0; i < LLC_SETS; i++) {
        //cout << ((~i>>6) & 0x1F) << " " << (i & 0x1F) << endl;
        // if (((i>>6) & 0x1F) == (i & 0x1F)) {
        //     //cout << "Hawkeye " << i << endl;
        //     hawkeye_sample[i] = 1;
        // }
        if (((~i>>6) & 0x1F) == (i & 0x1F)) {
            ship_sample[i] = 1;
            //cout << "Ship++ " << i << endl;
        }
    }

    // while (leaders < NUM_LEADER_SETS) {
    //     int randval = rand() % NUM_SET;

    //     if (ship_sample[randval] == 0 && SAMPLED_SET(randval) == 0) {
    //         ship_sample[randval] = 1;
    //         leaders++;
    //     }
    // }
}

uint32_t find_victim_hawkeye(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK* current_set, uint64_t PC, uint64_t paddr, uint32_t type)
{
    // look for the maxRRPV line
    for (uint32_t i = 0; i < LLC_WAYS; i++)
        if (rrpv_hawkeye[set][i] == maxRRPV_hawkeye)
        // if (rrpv[set][i] == maxRRPV)
            return i;

    // If we cannot find a cache-averse line, we evict the oldest cache-friendly line
    uint32_t max_rrip = 0;
    int32_t lru_victim = -1;
    for (uint32_t i = 0; i < LLC_WAYS; i++) {
        if (rrpv_hawkeye[set][i] >= max_rrip) {
        // if (rrpv[set][i] >= max_rrip) {
            max_rrip = rrpv_hawkeye[set][i];
            // max_rrip = rrpv[set][i];
            lru_victim = i;
        }
    }

    assert(lru_victim != -1);
    // The predictor is trained negatively on LRU evictions
    if (SAMPLED_SET(set)) {
    // if (hawkeye_sample[set]) {
        if (prefetched[set][lru_victim])
            prefetch_predictor->decrement(signatures[set][lru_victim]);
        else
            demand_predictor->decrement(signatures[set][lru_victim]);
    }
    return lru_victim;

    // WE SHOULD NOT REACH HERE
    assert(0);
    return 0;
}

uint32_t find_victim_shippp(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK* current_set, uint64_t PC, uint64_t paddr, uint32_t type)
{
    // look for the maxRRPV line
    while (1) {
        for (int i = 0; i < LLC_WAYS; i++)
            if (line_rrpv[set][i] == maxRRPV_shippp) { // found victim
            // if (rrpv[set][i] == maxRRPV) { // found victim
            return i;
        }

        for (int i = 0; i < LLC_WAYS; i++)
            line_rrpv[set][i]++;
            // rrpv[set][i]++;
    }

    // WE SHOULD NOT REACH HERE
    assert(0);
    return 0;
}

void replace_addr_history_element(unsigned int sampler_set)
{
  uint64_t lru_addr = 0;

  for (map<uint64_t, ADDR_INFO>::iterator it = addr_history[sampler_set].begin(); it != addr_history[sampler_set].end(); it++) {
    //     uint64_t timer = (it->second).last_quanta;

    if ((it->second).lru == (SAMPLER_WAYS - 1)) {
      // lru_time =  (it->second).last_quanta;
      lru_addr = it->first;
      break;
    }
  }

  addr_history[sampler_set].erase(lru_addr);
}

void update_addr_history_lru(unsigned int sampler_set, unsigned int curr_lru)
{
  for (map<uint64_t, ADDR_INFO>::iterator it = addr_history[sampler_set].begin(); it != addr_history[sampler_set].end(); it++) {
    if ((it->second).lru < curr_lru) {
      (it->second).lru++;
      assert((it->second).lru < SAMPLER_WAYS);
    }
  }
}

// called on every cache hit and cache fill
void update_replacement_state_hawkeye(uint32_t cpu, uint32_t set, uint32_t way, uint64_t paddr, uint64_t PC, uint64_t victim_addr, uint32_t type, uint8_t hit)
{
    paddr = (paddr >> 6) << 6;

    if (type == PREFETCH) {
        if (!hit)
        prefetched[set][way] = true;
    }
    else
        prefetched[set][way] = false;

    // Ignore writebacks
    if (type == WRITEBACK)
        return;

    // If we are sampling, OPTgen will only see accesses from sampled sets
    if (SAMPLED_SET(set)) {
    // if (hawkeye_sample[set]) {
        // The current timestep
        uint64_t curr_quanta = perset_mytimer[set] % OPTGEN_VECTOR_SIZE;

        uint32_t sampler_set = (paddr >> 6) % SAMPLER_SETS;
        uint64_t sampler_tag = CRC(paddr >> 12) % 256;
        assert(sampler_set < SAMPLER_SETS);

        // This line has been used before. Since the right end of a usage interval is always
        // a demand, ignore prefetches
        if ((addr_history[sampler_set].find(sampler_tag) != addr_history[sampler_set].end()) && (type != PREFETCH)) {
            unsigned int curr_timer = perset_mytimer[set];
            if (curr_timer < addr_history[sampler_set][sampler_tag].last_quanta)
                curr_timer = curr_timer + TIMER_SIZE;
            bool wrap = ((curr_timer - addr_history[sampler_set][sampler_tag].last_quanta) > OPTGEN_VECTOR_SIZE);
            uint64_t last_quanta = addr_history[sampler_set][sampler_tag].last_quanta % OPTGEN_VECTOR_SIZE;
            // and for prefetch hits, we train the last prefetch trigger PC
            if (!wrap && perset_optgen[set].should_cache(curr_quanta, last_quanta)) {
                if (addr_history[sampler_set][sampler_tag].prefetched)
                    prefetch_predictor->increment(addr_history[sampler_set][sampler_tag].PC);
                else
                    demand_predictor->increment(addr_history[sampler_set][sampler_tag].PC);
            }
            else {
                // Train the predictor negatively because OPT would not have cached this line
                if (addr_history[sampler_set][sampler_tag].prefetched)
                    prefetch_predictor->decrement(addr_history[sampler_set][sampler_tag].PC);
                else
                    demand_predictor->decrement(addr_history[sampler_set][sampler_tag].PC);
            }
            // Some maintenance operations for OPTgen
            perset_optgen[set].add_access(curr_quanta);
            update_addr_history_lru(sampler_set, addr_history[sampler_set][sampler_tag].lru);

            // Since this was a demand access, mark the prefetched bit as false
            addr_history[sampler_set][sampler_tag].prefetched = false;
        }
    
        // This is the first time we are seeing this line (could be demand or prefetch)
        else if (addr_history[sampler_set].find(sampler_tag) == addr_history[sampler_set].end()) {
            // Find a victim from the sampled cache if we are sampling
            if (addr_history[sampler_set].size() == SAMPLER_WAYS)
                replace_addr_history_element(sampler_set);

            assert(addr_history[sampler_set].size() < SAMPLER_WAYS);
            // Initialize a new entry in the sampler
            addr_history[sampler_set][sampler_tag].init(curr_quanta);
            // If it's a prefetch, mark the prefetched bit;
            if (type == PREFETCH) {
                addr_history[sampler_set][sampler_tag].mark_prefetch();
                perset_optgen[set].add_prefetch(curr_quanta);
            }
            else
                perset_optgen[set].add_access(curr_quanta);
            update_addr_history_lru(sampler_set, SAMPLER_WAYS - 1);
        }
        else // This line is a prefetch 
        {
            assert(addr_history[sampler_set].find(sampler_tag) != addr_history[sampler_set].end());
            // if(hit && prefetched[set][way])
            uint64_t last_quanta = addr_history[sampler_set][sampler_tag].last_quanta % OPTGEN_VECTOR_SIZE;
            if (perset_mytimer[set] - addr_history[sampler_set][sampler_tag].last_quanta < 5 * NUM_CORE) {
                if (perset_optgen[set].should_cache(curr_quanta, last_quanta)) {
                    if (addr_history[sampler_set][sampler_tag].prefetched)
                        prefetch_predictor->increment(addr_history[sampler_set][sampler_tag].PC);
                    else
                        demand_predictor->increment(addr_history[sampler_set][sampler_tag].PC);
                }
            }

            // Mark the prefetched bit
            addr_history[sampler_set][sampler_tag].mark_prefetch();
            // Some maintenance operations for OPTgen
            perset_optgen[set].add_prefetch(curr_quanta);
            update_addr_history_lru(sampler_set, addr_history[sampler_set][sampler_tag].lru);
        }

        // Get Hawkeye's prediction for this line
        bool new_prediction = demand_predictor->get_prediction(PC);
        if (type == PREFETCH)
            new_prediction = prefetch_predictor->get_prediction(PC);
        // Update the sampler with the timestamp, PC and our prediction
        // For prefetches, the PC will represent the trigger PC
        addr_history[sampler_set][sampler_tag].update(perset_mytimer[set], PC, new_prediction);
        addr_history[sampler_set][sampler_tag].lru = 0;
        // Increment the set timer
        perset_mytimer[set] = (perset_mytimer[set] + 1) % TIMER_SIZE;
    }

    bool new_prediction = demand_predictor->get_prediction(PC);
    if (type == PREFETCH)
        new_prediction = prefetch_predictor->get_prediction(PC);

    signatures[set][way] = PC;

    // Set RRIP values and age cache-friendly line
    if (!new_prediction)
        rrpv_hawkeye[set][way] = maxRRPV_hawkeye;
        // rrpv[set][way] = maxRRPV;
    else {
        rrpv_hawkeye[set][way] = 0;
        // rrpv[set][way] = 0;
        if (!hit) {
            bool saturated = false;
            for (uint32_t i = 0; i < LLC_WAYS; i++)
                if (rrpv_hawkeye[set][i] == maxRRPV_hawkeye - 1)
                // if (rrpv[set][i] == maxRRPV - 1)
            saturated = true;

            // Age all the cache-friendly  lines
            for (uint32_t i = 0; i < LLC_WAYS; i++) {
                if (!saturated && rrpv_hawkeye[set][i] < maxRRPV_hawkeye - 1)
                // if (!saturated && rrpv[set][i] < maxRRPV - 1)
                    rrpv_hawkeye[set][i]++;
                    // rrpv[set][i]++;
            }
        }
        rrpv_hawkeye[set][way] = 0;
        // rrpv[set][way] = 0;
    }
}

// called on every cache hit and cache fill
void update_replacement_state_shippp(uint32_t cpu, uint32_t set, uint32_t way, uint64_t paddr, uint64_t PC, uint64_t victim_addr, uint32_t type, uint8_t hit)
{
    uint32_t sig = line_sig[set][way];

    if (hit) { // update to REREF on hit
        if (type != WRITEBACK) {

            if ((type == PREFETCH) && is_prefetch[set][way]) {
                //                line_rrpv[set][way] = 0;

                if ((ship_sample[set] == 1) && (rand() % 100 < 5)) {
                    uint32_t fill_cpu = fill_core[set][way];

                    SHCT[fill_cpu][sig] = SAT_INC(SHCT[fill_cpu][sig], maxSHCTR);
                    line_reuse[set][way] = TRUE;
                }
            }
            else {
                line_rrpv[set][way] = 0;
                // rrpv[set][way] = 0;

                if (is_prefetch[set][way]) {
                    line_rrpv[set][way] = maxRRPV_shippp;
                    // rrpv[set][way] = maxRRPV;
                    is_prefetch[set][way] = FALSE;
                    total_prefetch_downgrades++;
                }

                if ((ship_sample[set] == 1) && (line_reuse[set][way] == 0)) {
                    uint32_t fill_cpu = fill_core[set][way];

                    SHCT[fill_cpu][sig] = SAT_INC(SHCT[fill_cpu][sig], maxSHCTR);
                    line_reuse[set][way] = TRUE;
                }
            }
        }

        return;
    }

    //--- All of the below is done only on misses -------
    // remember signature of what is being inserted
    uint64_t use_PC = (type == PREFETCH) ? ((PC << 1) + 1) : (PC << 1);
    uint32_t new_sig = use_PC % SHCT_SIZE_shippp;

    if (ship_sample[set] == 1) {
        uint32_t fill_cpu = fill_core[set][way];

        // update signature based on what is getting evicted
        if (line_reuse[set][way] == FALSE) {
            SHCT[fill_cpu][sig] = SAT_DEC(SHCT[fill_cpu][sig]);
        }
        else {
            SHCT[fill_cpu][sig] = SAT_INC(SHCT[fill_cpu][sig], maxSHCTR);
        }

        line_reuse[set][way] = FALSE;
        line_sig[set][way] = new_sig;
        fill_core[set][way] = cpu;
    }

    is_prefetch[set][way] = (type == PREFETCH);

    // Now determine the insertion prediciton

    uint32_t priority_RRPV = maxRRPV_shippp - 1; // default SHIP
    // uint32_t priority_RRPV = maxRRPV - 1; // default SHIP

    if (type == WRITEBACK) {
        line_rrpv[set][way] = maxRRPV_shippp;
        // rrpv[set][way] = maxRRPV;
    } else if (SHCT[cpu][new_sig] == 0) {
        line_rrpv[set][way] = (rand() % 100 >= RRIP_OVERRIDE_PERC) ? maxRRPV_shippp : priority_RRPV; // LowPriorityInstallMostly
        // rrpv[set][way] = (rand() % 100 >= RRIP_OVERRIDE_PERC) ? maxRRPV : priority_RRPV; // LowPriorityInstallMostly
    } else if (SHCT[cpu][new_sig] == maxSHCTR) {
        line_rrpv[set][way] = (type == PREFETCH) ? 1 : 0; // HighPriority Install
        // rrpv[set][way] = (type == PREFETCH) ? 1 : 0; // HighPriority Install
    } else {
        line_rrpv[set][way] = priority_RRPV; // HighPriority Install
        // rrpv[set][way] = priority_RRPV; // HighPriority Install
    }

    // Stat tracking for what insertion it was at
    insertion_distrib[type][line_rrpv[set][way]]++;
    // insertion_distrib[type][rrpv[set][way]]++;
}

void CACHE::initialize_replacement() {

    cout << "5th iteration" << std::endl;
    cout << "No hysteresis, common rrpv" << std::endl;

    initialize_replacement_hawkeye();
    initialize_replacement_shippp(NUM_SET);
    //cout << "NUM_SETs: " << NUM_SET << std::endl;
    //cout << "NUM_WAYs: " << NUM_WAY << std::endl;
}

// uint32_t CACHE::find_victim(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK* current_set, uint64_t PC, uint64_t paddr, uint32_t type) {
    
//     uint32_t select_val = 0;
//     uint32_t victim_val = 0;

//     if (hawkeye_sample[set] == 1) select_val = 1;  //Hawkeye sampled cache line
//     else if (ship_sample[set] == 1) select_val = 0; //Ship++ sampled cache line
//     else {
//         //if (select_sat_counter < (maxCNTRval/2) - (hysteresis/2)) select_val = 0;
//         if (select_sat_counter < (maxCNTRval/2)) select_val = 0;
//         //else if (select_sat_counter > (maxCNTRval/2) + (hysteresis/2) + 1) select_val = 1;
//         else if (select_sat_counter >= (maxCNTRval/2)) select_val = 1;
//         //else {
//         //    if (hysteresis_check == 0) select_val = 0;
//         //    else select_val = 1;
//         //}
//     }

//     if (select_val == 1) victim_val = find_victim_hawkeye(cpu, instr_id, set, current_set, PC, paddr, type);
//     else victim_val = find_victim_shippp(cpu, instr_id, set, current_set, PC, paddr, type);

//     //std::cout << "Prediction: " << select_val << std::endl;

//     return victim_val;
// }

// void CACHE::update_replacement_state(uint32_t cpu, uint32_t set, uint32_t way, uint64_t full_addr, uint64_t ip, uint64_t victim_addr, uint32_t type, uint8_t hit) {
    
//     uint32_t select_val = 0;

//     if (hawkeye_sample[set] == 1) select_val = 1;  //Hawkeye sampled cache line
//     else if (ship_sample[set] == 1) select_val = 0; //Ship++ sampled cache line
//     else select_val = 2;

//     if (select_val == 0) {
//         if (select_sat_counter > 0) {
//             if (hit == 0) {
//                 select_sat_counter+=1;
//             }
//             //if (select_sat_counter > (maxCNTRval/2) + (hysteresis/2) + 1) {
//             //    hysteresis_check = 1;
//             //}
//             //else hysteresis_check = 0;
//         }
//         update_replacement_state_shippp(cpu, set, way, full_addr, ip, victim_addr, type, hit);
//     }
//     else if (select_val == 1) {
//         if (select_sat_counter < maxCNTRval) {
//             if (hit == 0) {
//                 select_sat_counter-=1;
//             }
//             //if (select_sat_counter < (maxCNTRval/2) - (hysteresis/2)) {
//             //    hysteresis_check = 0;
//             //}
//             //else hysteresis_check = 1;
//         }
//         update_replacement_state_shippp(cpu, set, way, full_addr, ip, victim_addr, type, hit);
//     }

//     //std::cout << "Counter: " << select_sat_counter << std::endl;
// }

uint32_t CACHE::find_victim(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK* current_set, uint64_t PC, uint64_t paddr, uint32_t type) {
    
    if (hawkeye_sample_dip[set] == 1) return find_victim_hawkeye(cpu, instr_id, set, current_set, PC, paddr, type);
    else if (ship_sample_dip[set] == 1) return find_victim_shippp(cpu, instr_id, set, current_set, PC, paddr, type);
    else {
        if (select_sat_counter < (maxCNTRval/2)) return find_victim_shippp(cpu, instr_id, set, current_set, PC, paddr, type);
        else return find_victim_hawkeye(cpu, instr_id, set, current_set, PC, paddr, type);
    }

    // else {
    //     if (select_sat_counter < (maxCNTRval/2) - hysteresis) return find_victim_shippp(cpu, instr_id, set, current_set, PC, paddr, type);
    //     else if (select_sat_counter > (maxCNTRval/2) + hysteresis) return find_victim_hawkeye(cpu, instr_id, set, current_set, PC, paddr, type);
    //     else {
    //         if (hysteresis_check) return find_victim_hawkeye(cpu, instr_id, set, current_set, PC, paddr, type);
    //         else return find_victim_shippp(cpu, instr_id, set, current_set, PC, paddr, type);
    //     }
    // }

}

void CACHE::update_replacement_state(uint32_t cpu, uint32_t set, uint32_t way, uint64_t full_addr, uint64_t ip, uint64_t victim_addr, uint32_t type, uint8_t hit) {

    // if (!hawkeye_sample[set]) update_replacement_state_shippp(cpu, set, way, full_addr, ip, victim_addr, type, hit);
    // if (!ship_sample[set]) update_replacement_state_hawkeye(cpu, set, way, full_addr, ip, victim_addr, type, hit);

    if (hawkeye_sample[set] == 1) update_replacement_state_hawkeye(cpu, set, way, full_addr, ip, victim_addr, type, hit);
    else if (ship_sample[set] == 1) update_replacement_state_shippp(cpu, set, way, full_addr, ip, victim_addr, type, hit);
    else {
        if (select_sat_counter < (maxCNTRval/2)) update_replacement_state_shippp(cpu, set, way, full_addr, ip, victim_addr, type, hit);
        else update_replacement_state_hawkeye(cpu, set, way, full_addr, ip, victim_addr, type, hit);
    }

    if (hit == 0) {
        if (hawkeye_sample[set] && select_sat_counter > 0) {
            select_sat_counter--;
            //if (select_sat_counter < (maxCNTRval/2) - hysteresis) hysteresis_check = 0;
        }
        else if (ship_sample[set] && select_sat_counter < maxCNTRval) {
            select_sat_counter++;
            //if (select_sat_counter > (maxCNTRval/2) + hysteresis) hysteresis_check = 1;
        }
    }

    if (!hit && hawkeye_sample[set]) hawkeye_misses++;
    if (!hit && ship_sample[set]) shippp_misses++;

    //std::cout << "Ship++ misses: " << shippp_misses << ", " << "Hawkeye misses: " << hawkeye_misses << ", " << "Counter: " << select_sat_counter << std::endl;
}

void CACHE::replacement_final_stats() {}