#include "cachelab.h"
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <memory.h>
#include <errno.h>

typedef unsigned long long ull;

/**
 * csum configuration
 */
typedef struct {
    bool verbose;
    ull sets; /**< Number of sets */
    ull lines; /**< Number of lines per set */
    ull block_size; /**< block size */
    FILE* trace_file;
} config_t;

/**
 * @brief cache line in the set
 */
typedef struct {
    bool valid;
    ull tag;
    uint8_t* block;
} line_t;

/**
 * @brief one set in a cache
 */
typedef struct {
    line_t* lines; 
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
                int set_index = atol(optarg);
                if (set_index < 0 || set_index >= 64) {
                    fprintf(stderr, "The number of set index bits should be 0 <= index < 32\n");
                    return -2;
                }
                config->sets = 1 << set_index;
                break;
            }
            case 'E': {
                long lines = atol(optarg);
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

result_t run(config_t* config, cache_t* cache) {
    result_t res = {0, 0, 0};
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
