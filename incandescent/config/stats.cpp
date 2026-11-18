

#include "stats.h"

#include "../gl/envvars.h"
#include "../gl/log.h"
#include "../gl/inc.h"
#include "cJSON.h"
#include "config.h"
#include <climits>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEBUG 0

char* stats_file_path = nullptr;

static const char* LAUNCH_COUNT_KEY = "launchCount";

static const char* COUNT_ENV_VAR = "INC_COUNT_LAUNCH";

static cJSON* read_stats() {
    FILE* file = fopen(stats_file_path, "r");
    if (!file) return nullptr;

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        fclose(file);
        return nullptr;
    }

    char* content = (char*)malloc(size + 1);
    if (!content) {
        fclose(file);
        return nullptr;
    }

    size_t read = fread(content, 1, size, file);
    fclose(file);
    content[read] = '\0';

    cJSON* json = cJSON_Parse(content);
    free(content);

    if (json && !cJSON_IsObject(json)) {
        cJSON_Delete(json);
        return nullptr;
    }
    return json;
}

static void write_stats(cJSON* json) {
    char* text = cJSON_PrintUnformatted(json);
    if (!text) return;

    FILE* file = fopen(stats_file_path, "w");
    if (file) {
        fwrite(text, 1, strlen(text), file);
        fclose(file);
    } else {
        LOG_D("Unable to write stats file %s", stats_file_path);
    }
    cJSON_free(text);
}

void bump_launch_count() {
    if (!IsEnvVarTrue(COUNT_ENV_VAR)) {
        LOG_D("%s is not set, not counting this load as a launch", COUNT_ENV_VAR);
        return;
    }

    if (!stats_file_path) {
        LOG_D("Stats path is unknown, not counting this launch");
        return;
    }

    cJSON* json = read_stats();
    if (!json) json = cJSON_CreateObject();
    if (!json) return;

    int count = 0;
    cJSON* item = cJSON_GetObjectItem(json, LAUNCH_COUNT_KEY);
    if (item && cJSON_IsNumber(item)) count = item->valueint;

    if (count < 0 || count == INT_MAX) count = 0;

    cJSON_DeleteItemFromObject(json, LAUNCH_COUNT_KEY);
    cJSON_AddNumberToObject(json, LAUNCH_COUNT_KEY, count + 1);

    write_stats(json);
    cJSON_Delete(json);

    LOG_D("Launch count is now %d", count + 1);
}
