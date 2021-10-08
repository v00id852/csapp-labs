#include "cachelab.h"
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <memory.h>
#include <errno.h>

#define MAX_LEN 100

typedef unsigned long long ull;

/**
 * csum configuration
 */
typedef struct {
    bool verbose;
    ull sets; /**< Number of sets */
    ull lines; /**< Number of lines per set */
    ull block_size; /**< block size */

    ull set_bits;
    ull block_bit;
    FILE* trace_file;
} config_t;

/**
 * @brief cache line in the set
 */
typedef struct {
    bool valid;
    line_t *prev, *next;
    ull tag;
    uint8_t* block;
} line_t;

/**
 * @brief one set in a cache
 */
typedef struct {
    line_t* lines; 
    line_t head, tail;
} set_t;

/**
 * @brief a cache
 */
typedef struct {
    set_t* sets;
} cache_t;

typedef struct {
    int hit_count;
    int miss_count;
    int eviction_count;
} result_t;

typedef struct {
    int op;
    ull addr;
    int size;
} trace_t;

void usage() {
    printf("./csim [-hv] -s <s> -E <E> -b <b> -t <tracefile>\n");
}

void printConfig(config_t* config) {
    if (!config) return;
    printf("sets: %llu, lines: %llu, block_size: %llu, verbose: %d\n",
        config->sets, config->lines, config->block_size, config->verbose);
}

int parseOpt(int argc, char* argv[], config_t* config) {
    if (!config) return -1;
    int opt;
	while((opt = getopt(argc, argv, "hvs:E:b:t:")) != -1) {
		switch (opt) {
            case 'h':
                usage();
                return 0;
            case 'v':
                config->verbose = true;
                break;
            case 's': {
                int set_index = strtol(optarg, NULL, 10);
                if (set_index < 0 || set_index >= 64) {
                    fprintf(stderr, "The number of set index bits should be 0 <= index < 32\n");
                    return -2;
                }
                config->sets = 1 << set_index;
                config->set_bits = set_index;
                break;
            }
            case 'E': {
                long lines = strtol(optarg, NULL, 10);
                if (lines < 0) {
                    fprintf(stderr, "The number of lines per set should be greater than zero\n");
                    return -3;
                }
                config->lines = lines;
                break;
            }
            case 'b': {
                int block_index = atol(optarg);
                if (block_index < 0 || block_index >= 64) {
                    fprintf(stderr, "The number of block bits should be 0 <= bits < 64\n");
                    return -4;
                }
                config->block_size = 1 << block_index;
                config->block_bit = block_index;
                break;
            }
            case 't': {
                FILE *file = fopen(optarg, "r");
                if (file == NULL) {
                    fprintf(stderr, "Invalid file path\n");
                    return -5;
                }
                config->trace_file = file;
                break;
            }
            default:
                usage();
                break;
		}
	}
    return 0;
}

int createCache(cache_t* cache, config_t* config) {
    if (!cache) return -1;
    cache->sets = malloc(config->sets * sizeof(set_t));
    if (!cache->sets) {
        fprintf(stderr, "allocate sets failed: %s\n", strerror(errno));
        return -2;
    }
    memset(cache->sets, 0, sizeof(set_t) * config->sets);

    for(ull i = 0; i < config->sets; ++i) {
        cache->sets[i].lines = malloc(config->lines * sizeof(line_t));
        if (!cache->sets[i].lines) {
            fprintf(stderr, "allocate sets[%llu].lines failed: %s\n", 
                i, strerror(errno));
            goto destroy;
        }
        for(ull j = 0; j < config->lines; ++j) {
            cache->sets[i].lines[j].block = malloc(config->block_size);
            if (!cache->sets[i].lines[j].block) {
                fprintf(stderr, "allocate sets[%llu].lines[%llu] block failed: %s\n", 
                    i, j, strerror(errno));
                goto destroy;
            }
        }
        cache->sets[i].head.next = &cache->sets[i].tail;
        cache->sets[i].head.prev = NULL;
        cache->sets[i].tail.prev = &cache->sets[i].head;
        cache->sets[i].tail.next = NULL;
    }
    return 0;
destroy:
    
    for (ull i = 0; i < config->sets; ++i) {            
        if (!cache->sets[i].lines) continue;
        for (ull j = 0; j < config->lines; ++j) {
            if (!cache->sets[i].lines[j].block) continue;
            free(cache->sets[i].lines[j].block);
        }
        free(cache->sets[i].lines);
    }

    free(cache->sets);
    return -1;
} 

void destroyCache(cache_t* cache, config_t* config) {
    if (!cache) return;
    if (!cache->sets) return;
    for(ull i = 0; i < config->sets; ++i) {
        for(ull j = 0; j < config->lines; ++j) {
            if (!cache->sets[i].lines) continue;
            free(cache->sets[i].lines[j].block);
        }
        free(cache->sets[i].lines);
    }
    free(cache->sets);
}

/**
 * Parse a valgrid trace, ignore I operation
 * 
 * format: 
 * I 0400d7d4,8
 *  M/L/S 0421c7f0,4
 */
trace_t parse_trace(char* buf) {
    trace_t trace = {0, 0, 0};
    if (buf[0] == 'I') return trace;
    if (buf[1] != 'M' && buf[1] != 'L' && buf[1] != 'S') return trace;
    trace.op = buf[1];
    trace.addr = strtol(buf + 3, NULL, 16);
    trace.size = strtol(buf + 10, NULL, 10);
    return trace;
}

static line_t* find_a_empty_line(cache_t* cache, config_t* config, unsigned set_index) {
    line_t* line = NULL;
    for(int i = 0; i < config->lines; ++i) {
        line = &(cache->sets[set_index].lines[i]);
        if (!line->valid) return line;
    }
    return NULL;
}

static void lru_move_to_head(set_t* set, line_t* line) {
    line_t* head = &set->head;
    line_t* tail = &set->tail;
    if (line->prev)
        line->prev->next = line->next;
    if (line->next)
        line->next->prev = line->prev;

    // insert after head;
    line->next = head->next;
    head->next->prev = line;
    line->prev = head;
    head->next = line;
}

/**
 * Use LRU to evict a line
 */
static void evict(set_t* set) {
    // evict the last one;
    line_t* last = &set->tail.prev;
    lru_move_to_head(set, last);
} 

/**
 * Simulate a cache.
 * 
 * Step1: get set index, line tag and block index from the address in the trace.
 * Step2: judge if the line(s) are valid, and whether the line tag is the same with the tag in the address.
 * Step3: if tags are same, hit, or miss. In the miss situation, depends on the operation, 
 *        if the operation is load(L), the cache should load from a lower level cache (one miss plus a possible eviction), 
 *        if the operation is store(S), one miss plus a possbile eviction (write-allocation),
 *        if the operation is modify(M), it can be treated as a load followed by a store, so it may result in two cache hits 
 *        (one load and one store), or a miss and a hit plus a possible eviction (load miss, eviction, and store hit).
 */
void simulate(trace_t* trace, cache_t* cache,
                config_t* config, result_t* res) {
    if (!trace || !cache || !config || !res) return;
    if (trace->op == 0) return;
    // Step1, get set index, line tag and block index from address.
    ull addr = trace->addr;
    ull block_index = addr & ((ull)-1 >> (sizeof(ull) - config->block_bit));
    ull set_index = (addr & ((ull)-1 >> (sizeof(ull) - config->block_bit - config->set_bits))) >> config->block_bit;
    ull tag = (addr >> (config->block_bit + config->set_bits)) & ((unsigned)-1 >> (config->block_bit + config->set_bits));
    // Step2
    bool miss = true;
    line_t* line = NULL;
    for(int i = 0; i < config->lines; ++i) {
        line_t* line = &(cache->sets[set_index].lines[i]);
        if (line->valid && tag == line->tag) {
            miss = false;
            break;
        }
    }
    // Step3
    // hit situation
    printf("%c %ul,%d ", trace->op, trace->addr, trace->size);
    if (!miss) {
        if (trace->op == 'M') {
            res->hit_count+=2;
            if (config->verbose)
                printf("hit hit\n");
        } else {
            res->hit_count++;
            if (config->verbose) {
                printf("hit\n", trace->op, trace->addr, trace->size);
            }
        }
        lru_move_to_head(&cache->sets[set_index], line);
        return;
    }
    // miss situration
    res->miss_count++;
    switch(trace->op) {
        case 'L':
            // load instruction, the cache should load from the lower cache.
            // find a empty line
            {
                line_t* line = find_a_empty_line(cache, config, set_index);
                if (!line) {
                    // eviction
                    res->eviction_count ++;
                    evict(&cache->sets[set_index]);
                } else {
                    lru_move_to_head(&cache->sets[set_index], line);
                }
                if (config->verbose) {
                    if (line) {
                        printf("L %d,%d miss\n", trace->addr, trace->size);
                    } else {
                        printf("L %d,%d miss eviction\n", trace->addr, trace->size);
                    }
                }
                break;
            }
        case 'S':
            // store instruction, if miss, find a empty line
            line_t* line = find_a_empty_line(cache, config, set_index);
            if (!line) {
                res->eviction_count ++;
                evict(&cache->sets[set_index]);
            } else {
                lru_move_to_head(&cache->sets[set_index], line);
            }
            if (config->verbose) {
                if (line) {
                    printf("S %d,%d miss\n", trace->addr, trace->size);
                } else {
                    printf("S %d,%d miss eviction\n", trace->addr, trace->size);
                }
            }
            break;
        case 'M':
            // modify instruction, load and store.
            {
                printf("M %d,%d miss ", trace->addr, trace->size);
                // load first;
                bool found = false;
                for(int i = 0; i < config->lines; ++i) {
                    line = &(cache->sets[set_index].lines[i]);
                    if (!line->valid) {
                        // find a empty line, store data
                        line->valid = true;
                        line->tag = tag;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    // eviction
                    res->eviction_count++;
                    evict(&cache->sets[set_index]);
                } else {
                    lru_move_to_head(&cache->sets[set_index], line);
                }
                
                res->hit_count++;

                if (config->verbose) {
                    if (found) {
                        printf("hit\n");
                    } else {
                        printf("eviction hit\n");
                    }
                }
                break;
            }
        default: break;
    }
} 

result_t run(config_t* config, cache_t* cache) {
    result_t res = {0, 0, 0};
    char buf[MAX_LEN] = {0};

    while(fgets(buf, sizeof(buf), config->trace_file) != NULL) {
        trace_t trace = parse_trace(buf);
        sumulate(&trace, &cache, &config, &res);
    }

    return res;
}

int main(int argc, char* argv[])
{
    config_t config;
    cache_t cache;
    result_t result;

    if (parseOpt(argc, argv, &config) != 0) {
        usage();
        return 0;
    }

    if (config.lines == 0 
        || config.sets == 0
        || config.block_size == 0
        || config.trace_file == NULL) {
        usage();
        return -1;
    }

    printConfig(&config);
    if (createCache(&cache, &config) < 0) {
        return -1;
    }
    
    result = run(&config, &cache);
    printSummary(result.hit_count, result.miss_count, result.eviction_count);
    return 0;
}
