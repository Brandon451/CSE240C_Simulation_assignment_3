//Set duelling implemented for Hawkeye and SHiP++
//Brandon Saldanha

#include "cache.h"

#include <map>

#define NUM_CORE 1
#define LLC_SETS 8192
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
uint32_t line_rrpv[MAX_LLC_SETS][LLC_WAYS];
uint32_t is_prefetch[MAX_LLC_SETS][LLC_WAYS];
uint32_t fill_core[MAX_LLC_SETS][LLC_WAYS];

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

uint32_t ship_sample[MAX_LLC_SETS];
uint32_t line_reuse[MAX_LLC_SETS][LLC_WAYS];
uint64_t line_sig[MAX_LLC_SETS][LLC_WAYS];

// Hawkeye Predictors for demand and prefetch requests
// Predictor with 2K entries and 5-bit counter per entry
// Budget = 2048*5/8 bytes = 1.2KB
#define MAX_SHCT 31
#define SHCT_SIZE_BITS 12
#define SHCT_SIZE_hawkeye (1 << SHCT_SIZE_BITS)
#include "hawkeye_predictor.h"
HAWKEYE_PC_PREDICTOR* demand_predictor;   // Predictor
HAWKEYE_PC_PREDICTOR* prefetch_predictor; // Predictor

// SHCT. Signature History Counter Table
// per-core 16K entry. 14-bit signature = 16k entry. 3-bit per entry
#define maxSHCTR 7
#define SHCT_bits 14
#define SHCT_SIZE_shipp (1 << SHCT_bits)
uint32_t SHCT[NUM_CORE][SHCT_SIZE_shippp];

#define OPTGEN_VECTOR_SIZE 128
#include "optgen.h"
OPTgen perset_optgen[LLC_SETS]; // per-set occupancy vectors; we only use 64 of these

// Statistics
uint64_t insertion_distrib[NUM_TYPES][maxRRPV + 1];
uint64_t total_prefetch_downgrades;

#include <math.h>
#define bitmask(l) (((l) == 64) ? (unsigned long long)(-1LL) : ((1LL << (l)) - 1LL))
#define bits(x, i, l) (((x) >> (i)) & bitmask(l))
// Sample 64 sets per core
#define SAMPLED_SET(set) (bits(set, 0, 6) == bits(set, ((unsigned long long)log2(LLC_SETS) - 6), 6))

// Sampler to track 8x cache history for sampled sets
// 2800 entris * 4 bytes per entry = 11.2KB
#define SAMPLED_CACHE_SIZE 2800
#define SAMPLER_WAYS 8
#define SAMPLER_SETS SAMPLED_CACHE_SIZE / SAMPLER_WAYS
vector<map<uint64_t, ADDR_INFO>> addr_history; // Sampler

#define maxCNTRval = 0xFFFFFFFF;

uint32_t select_sat_counter = maxCNTRval/2;

// initialize replacement state hawkeye
void initialize_replacement()
{
    for (int i = 0; i < LLC_SETS; i++) {
        for (int j = 0; j < LLC_WAYS; j++) {
            rrpv[i][j] = maxRRPV;
            signatures[i][j] = 0;
            prefetched[i][j] = false;
        }
        perset_mytimer[i] = 0;
        perset_optgen[i].init(LLC_WAYS - 2);
    }

    addr_history.resize(SAMPLER_SETS);
    for (int i = 0; i < SAMPLER_SETS; i++)
        addr_history[i].clear();

    demand_predictor = new HAWKEYE_PC_PREDICTOR();
    prefetch_predictor = new HAWKEYE_PC_PREDICTOR();

    cout << "Initialize Hawkeye state" << endl;
}

// initialize replacement state
void initialize_replacement_shippp()
{
  //int LLC_SETS = (get_config_number() <= 2) ? 2048 : MAX_LLC_SETS;

    cout << "Initialize SRRIP state" << endl;

    for (int i = 0; i < MAX_LLC_SETS; i++) {
        for (int j = 0; j < LLC_WAYS; j++) {
            line_rrpv[i][j] = maxRRPV;
            line_reuse[i][j] = FALSE;
            is_prefetch[i][j] = FALSE;
            line_sig[i][j] = 0;
        }
  }

    for (int i = 0; i < NUM_CORE; i++) {
        for (int j = 0; j < SHCT_SIZE; j++) {
            SHCT[i][j] = 1; // Assume weakly re-use start
        }
    }

    int leaders = 0;

    while (leaders < NUM_LEADER_SETS) {
        int randval = rand() % NUM_SET;

        if (ship_sample[randval] == 0 && SAMPLED_SET(randval) == 0) {
            ship_sample[randval] = 1;
            leaders++;
        }
    }
}

uint32_t find_victim_hawkeye(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK* current_set, uint64_t PC, uint64_t paddr, uint32_t type)
{
    // look for the maxRRPV line
    for (uint32_t i = 0; i < LLC_WAYS; i++)
        if (rrpv_hawkeye[set][i] == maxRRPV_hawkeye)
            return i;

    // If we cannot find a cache-averse line, we evict the oldest cache-friendly line
    uint32_t max_rrip = 0;
    int32_t lru_victim = -1;
    for (uint32_t i = 0; i < LLC_WAYS; i++) {
        if (rrpv_hawkeye[set][i] >= max_rrip) {
            max_rrip = rrpv_hawkeye[set][i];
            lru_victim = i;
        }
    }

    assert(lru_victim != -1);
    // The predictor is trained negatively on LRU evictions
    if (SAMPLED_SET(set)) {
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
            if (line_rrpv[set][i] == maxRRPV) { // found victim
            return i;
        }

        for (int i = 0; i < LLC_WAYS; i++)
            line_rrpv[set][i]++;
    }

    // WE SHOULD NOT REACH HERE
    assert(0);
    return 0;
}

void CACHE::initialize_replacement() {
    initialize_replacement_hawkeye();
    initialize_replacement_shippp();
}

uint32_t CACHE::find_victim(uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK* current_set, uint64_t ip, uint64_t full_addr, uint32_t type) {
    
    unint32_t select_val = 0;
    unint32_t victim_val = 0;

    if (SAMPLED_SET(set) == 1) select_val = 1;  //Hawkeye sampled cache line
    else if (ship_sample[set] == 1) select_val = 0; //Ship++ sampled cache line
    else {
        if (select_sat_counter < maxCNTRval/2) select_val = 1;
        else select_val = 0;
    }

    if (select_val == 0) find_victim_hawkeye(cpu, instr_id, set, current_set, PC, paddr, type);
    else find_victim_shippp(cpu, instr_id, set, current_set, PC, paddr, type);
}

void CACHE::update_replacement_state(uint32_t cpu, uint32_t set, uint32_t way, uint64_t full_addr, uint64_t ip, uint64_t victim_addr, uint32_t type, uint8_t hit) {}

void CACHE::replacement_final_stats() {}