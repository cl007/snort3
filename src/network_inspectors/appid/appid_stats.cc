//--------------------------------------------------------------------------
// Copyright (C) 2014-2016 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2005-2013 Sourcefire, Inc.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

// appid_stats.cc author Sourcefire Inc.

#include "appid_stats.h"

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <cstdint>

#include "log/messages.h"
#include "log/unified2.h"
#include "utils/sflsq.h"
#include "utils/util.h"

#include "appid_module.h"
#include "appid_api.h"
#include "appid_session.h"
#include "app_info_table.h"
#include "appid_utils/fw_avltree.h"

#define URLCATBUCKETS   100
#define URLREPBUCKETS   5

struct AppIdStatRecord
{
    uint32_t app_id;
    uint32_t initiatorBytes;
    uint32_t responderBytes;
};

#ifdef WIN32
#pragma pack(push,app_stats,1)
#else
#pragma pack(1)
#endif

struct AppIdStatOutputRecord
{
    char app_name[MAX_EVENT_APPNAME_LEN];
    uint32_t initiatorBytes;
    uint32_t responderBytes;
};

#ifdef WIN32
#pragma pack(pop,app_stats)
#else
#pragma pack()
#endif

struct StatsBucket
{
    uint32_t startTime;
    FwAvlTree* appsTree;
    struct
    {
        size_t txByteCnt;
        size_t rxByteCnt;
    } totalStats;
    uint32_t appRecordCnt;
};

static THREAD_LOCAL SF_LIST* currBuckets = nullptr;
static THREAD_LOCAL SF_LIST* logBuckets = nullptr;
static THREAD_LOCAL FILE* appfp = nullptr;
static THREAD_LOCAL size_t appSize;
static THREAD_LOCAL time_t appTime;
static THREAD_LOCAL const char* appid_stats_filename = nullptr;
static THREAD_LOCAL time_t bucketStart;
static THREAD_LOCAL time_t bucketInterval;
static THREAD_LOCAL time_t bucketEnd;

static const char appid_stats_file_suffix[] = "appid_stats.log";
static size_t rollSize;
static time_t rollPeriod;
static bool enableAppStats;

static void end_stats_period(void);
static void start_stats_period(time_t startTime);
static struct StatsBucket* get_stats_bucket(time_t startTime);
static void dump_statistics(void);

static void delete_record(void* record)
{
	snort_free(record);
}

static inline time_t get_time()
{
    auto now = time(nullptr);
    return now - (now % bucketInterval);
}

void update_appid_statistics(AppIdSession* asd)
{
    if ( !enableAppStats )
        return;

    time_t now = get_time();

    if (now >= bucketEnd)
    {
        end_stats_period();
        dump_statistics();
        start_stats_period(now);
    }

    time_t bucketTime = asd->stats.firstPktsecond -
        (asd->stats.firstPktsecond % bucketInterval);

    StatsBucket* bucket = get_stats_bucket(bucketTime);
    if ( !bucket )
        return;

    bucket->totalStats.txByteCnt += asd->stats.initiatorBytes;
    bucket->totalStats.rxByteCnt += asd->stats.responderBytes;

    const uint32_t web_app_id = asd->pick_payload_app_id();
    if (web_app_id > APP_ID_NONE)
    {
        const uint32_t app_id = web_app_id;
        AppIdStatRecord* record = (AppIdStatRecord*)fwAvlLookup(app_id, bucket->appsTree);
        if ( !record )
        {
            record = (AppIdStatRecord*)snort_calloc(sizeof(struct AppIdStatRecord));
            if (fwAvlInsert(app_id, record, bucket->appsTree) == 0)
            {
                record->app_id = app_id;
                bucket->appRecordCnt += 1;
#ifdef DEBUG_STATS
                fprintf(SF_DEBUG_FILE, "New App: %u Count %u\n", record->app_id,
                    bucket->appRecordCnt);
#endif
            }
            else
            {
                WarningMessage("Error saving statistics record for app id: %u", app_id);
                snort_free(record);
                record = nullptr;
            }
        }

        if (record)
        {
            record->initiatorBytes += asd->stats.initiatorBytes;
            record->responderBytes += asd->stats.responderBytes;
        }
    }

    const uint32_t service_app_id = asd->pick_service_app_id();
    if ((service_app_id) &&
        (service_app_id != web_app_id))
    {
        const uint32_t app_id = service_app_id;
        AppIdStatRecord* record = (AppIdStatRecord*)fwAvlLookup(app_id, bucket->appsTree);
        if ( !record )
        {
            record = (AppIdStatRecord*)snort_calloc(sizeof(struct AppIdStatRecord));
            if (fwAvlInsert(app_id, record, bucket->appsTree) == 0)
            {
                record->app_id = app_id;
                bucket->appRecordCnt += 1;
#ifdef DEBUG_STATS
                fprintf(SF_DEBUG_FILE, "New App: %u Count %u\n", record->app_id,
                    bucket->appRecordCnt);
#endif
            }
            else
            {
                WarningMessage("Error saving statistics record for app id: %u", app_id);
                snort_free(record);
                record = nullptr;
            }
        }

        if (record)
        {
            record->initiatorBytes += asd->stats.initiatorBytes;
            record->responderBytes += asd->stats.responderBytes;
        }
    }

    const uint32_t client_app_id = asd->pick_client_app_id();
    if (client_app_id > APP_ID_NONE
        && client_app_id != service_app_id
        && client_app_id != web_app_id)
    {
        const uint32_t app_id = client_app_id;

        AppIdStatRecord* record = (AppIdStatRecord*)fwAvlLookup(app_id, bucket->appsTree);
        if ( !record )
        {
            record = (AppIdStatRecord*)snort_calloc(sizeof(struct AppIdStatRecord));
            if (fwAvlInsert(app_id, record, bucket->appsTree) == 0)
            {
                record->app_id = app_id;
                bucket->appRecordCnt += 1;
#ifdef DEBUG_STATS
                fprintf(SF_DEBUG_FILE, "New App: %u Count %u\n", record->app_id,
                    bucket->appRecordCnt);
#endif
            }
            else
            {
                WarningMessage("Error saving statistics record for app id: %u", app_id);
                snort_free(record);
                record = nullptr;
            }
        }

        if (record)
        {
            record->initiatorBytes += asd->stats.initiatorBytes;
            record->responderBytes += asd->stats.responderBytes;
        }
    }
}

void init_appid_statistics(const AppIdModuleConfig& config)
{
    if (config.stats_logging_enabled)
    {
        enableAppStats = true;
        std::string stats_file;
        appid_stats_filename = snort_strdup(get_instance_file(stats_file, appid_stats_file_suffix));

        rollPeriod = config.app_stats_rollover_time;
        rollSize = config.app_stats_rollover_size;
        bucketInterval = config.app_stats_period;

        time_t now = get_time();
        start_stats_period(now);
        appfp = nullptr;
    }
    else
        enableAppStats = false;
}

static void close_stats_log_file()
{
    if (appfp)
    {
        fclose(appfp);
        appfp = nullptr;
    }
}

void flush_appid_statistics()
{
    if (!enableAppStats)
        return;

    time_t now = get_time();
    if (now >= bucketEnd)
    {
        end_stats_period();
        dump_statistics();
        start_stats_period(now);
    }
}

static void start_stats_period(time_t startTime)
{
    bucketStart = startTime;
    bucketEnd = bucketStart + bucketInterval;
}

static void end_stats_period(void)
{
    SF_LIST* bucketList = logBuckets;
    logBuckets = currBuckets;
    currBuckets = bucketList;
}

static StatsBucket* get_stats_bucket(time_t startTime)
{
    StatsBucket* bucket = nullptr;

    if ( !currBuckets )
    {
        currBuckets = sflist_new();
#       ifdef DEBUG_STATS
        fprintf(SF_DEBUG_FILE, "New Stats Bucket List\n");
#       endif
    }

    if ( !currBuckets )
        return nullptr;

    SF_LNODE* lNode = nullptr;
    StatsBucket* lBucket = nullptr;

    for ( lBucket = (StatsBucket*)sflist_first(currBuckets, &lNode); lNode && lBucket;
        lBucket = (StatsBucket*)sflist_next(&lNode) )
    {
        if (startTime == lBucket->startTime)
        {
            bucket = lBucket;
            break;
        }
        else if (startTime < lBucket->startTime)
        {
            bucket = (StatsBucket*)snort_calloc(sizeof(StatsBucket));
            bucket->startTime = startTime;
            bucket->appsTree = fwAvlInit();
            sflist_add_before(currBuckets, lNode, bucket);

#ifdef DEBUG_STATS
            fprintf(SF_DEBUG_FILE, "New Bucket Time: %u before %u\n",
                bucket->startTime, lBucket->startTime);
#endif
            break;
        }
    }

    if ( !lNode )
    {
        bucket = (StatsBucket*)snort_calloc(sizeof(StatsBucket));
        bucket->startTime = startTime;
        bucket->appsTree = fwAvlInit();
        sflist_add_tail(currBuckets, bucket);

#ifdef DEBUG_STATS
        fprintf(SF_DEBUG_FILE, "New Bucket Time: %u at tail\n", bucket->startTime);
#endif
    }

    return bucket;
}

static FILE* open_stats_log_file(const char* const filename, time_t tstamp)
{
    FILE* fp;
    char output_fullpath[512];
    time_t curr_time;

    if (tstamp)
        curr_time = tstamp;
    else
        curr_time = time(nullptr);

    snprintf(output_fullpath, sizeof(output_fullpath), "%s.%lu", filename, curr_time);
    LogMessage("Opening %s for AppId statistics logging.\n", output_fullpath);

    if ((fp = fopen(output_fullpath, "w")) == nullptr)
    {
        ErrorMessage("Unable to open output file \"%s\": %s\n for AppId statistics logging.",output_fullpath, strerror(errno));
    }
    return fp;
}

static FILE* rollover_stats_log_file(const char* const filename, FILE* const oldfp, time_t tstamp)
{
    fclose(oldfp);
    return open_stats_log_file(filename, tstamp);
}

static void dump_statistics()
{
    struct StatsBucket* bucket = nullptr;
    uint8_t* buffer;
    uint32_t* buffPtr;
    struct    FwAvlNode* node;
    struct AppIdStatRecord* record;
    Serial_Unified2_Header header;

    size_t buffSize;
    time_t currTime = time(nullptr);

    if (logBuckets == nullptr)
        return;

    while ((bucket = (struct StatsBucket*)sflist_remove_head(logBuckets)) != nullptr)
    {
        if (bucket->appRecordCnt)
        {
            buffSize = ( bucket->appRecordCnt * sizeof(struct AppIdStatOutputRecord) ) +
                ( 4 * sizeof(uint32_t) );
            header.type = UNIFIED2_IDS_EVENT_APPSTAT;
            header.length = buffSize - ( 2 * sizeof(uint32_t));
            buffer = (uint8_t*)snort_calloc(buffSize);
#           ifdef DEBUG_STATS
            fprintf(SF_DEBUG_FILE, "Write App Records %u Size: %zu\n",
                bucket->appRecordCnt, buffSize);
#           endif
        }
        else
            buffer = nullptr;

        if (buffer)
        {
            buffPtr = (uint32_t*)buffer;
            *buffPtr++ = htonl(header.type);
            *buffPtr++ = htonl(header.length);
            *buffPtr++ = htonl(bucket->startTime);
            *buffPtr++ = htonl(bucket->appRecordCnt);

            for (node = fwAvlFirst(bucket->appsTree); node != nullptr; node = fwAvlNext(node))
            {
                struct AppIdStatOutputRecord* recBuffPtr;
                const char* app_name;
                bool cooked_client = false;
                AppId app_id;
                char tmpBuff[MAX_EVENT_APPNAME_LEN];

                record = (struct AppIdStatRecord*)node->data;
                app_id = record->app_id;

                recBuffPtr = (struct AppIdStatOutputRecord*) buffPtr;

                if (app_id >= 2000000000)
                {
                    cooked_client = true;
                    app_id -= 2000000000;
                }

                AppInfoTableEntry* entry = AppInfoManager::get_instance().get_app_info_entry(app_id);
                if (entry)
                {
                    app_name = entry->app_name;
                    if (cooked_client)
                    {
                        snprintf(tmpBuff, MAX_EVENT_APPNAME_LEN, "_cl_%s", app_name);
                        tmpBuff[MAX_EVENT_APPNAME_LEN-1] = 0;
                        app_name = tmpBuff;
                    }
                }
                else if (app_id == APP_ID_UNKNOWN || app_id == APP_ID_UNKNOWN_UI)
                    app_name = "__unknown";
                else if (app_id == APP_ID_NONE)
                    app_name = "__none";
                else
                {
                    if (cooked_client)
                        snprintf(tmpBuff, MAX_EVENT_APPNAME_LEN, "_err_cl_%u",app_id);
                    else
                        snprintf(tmpBuff, MAX_EVENT_APPNAME_LEN, "_err_%u",app_id);

                    tmpBuff[MAX_EVENT_APPNAME_LEN - 1] = 0;
                    app_name = tmpBuff;
                }

                memcpy(recBuffPtr->app_name, app_name, strlen(app_name));

                /**buffPtr++ = htonl(record->app_id); */
                recBuffPtr->initiatorBytes = htonl(record->initiatorBytes);
                recBuffPtr->responderBytes = htonl(record->responderBytes);
                buffPtr += sizeof(*recBuffPtr)/sizeof(*buffPtr);
            }

            if (appid_stats_filename)
            {
                if (!appfp)
                {
                    appfp = open_stats_log_file(appid_stats_filename, currTime);
                    appTime = currTime;
                    appSize = 0;
                }
                else if (((currTime - appTime) > rollPeriod) ||
                    ((appSize + buffSize) > rollSize))
                {
                    appfp = rollover_stats_log_file(appid_stats_filename, appfp, currTime);
                    appTime = currTime;
                    appSize = 0;
                }
                if (appfp)
                {
                    if ((fwrite(buffer, buffSize, 1, appfp) == 1) && (fflush(appfp) == 0))
                    {
                        appSize += buffSize;
                    }
                    else
                    {
                        ErrorMessage(
                            "AppID ailed to write to statistics file (%s): %s\n",
                            appid_stats_filename, strerror(errno));
                        fclose(appfp);
                        appfp = nullptr;
                    }
                }
            }
            snort_free(buffer);
        }
        fwAvlDeleteTree(bucket->appsTree, delete_record);
        snort_free(bucket);
    }
}

void cleanup_appid_statistics()
{
    if (!enableAppStats)
        return;

    /*flush the last stats period. */
    end_stats_period();
    dump_statistics();
    close_stats_log_file();
    snort_free((void*)appid_stats_filename);

    if (logBuckets)
        snort_free(logBuckets);

    if (currBuckets)
    {
    	while (auto bucket = (StatsBucket*)sflist_remove_head(currBuckets))
    	{
    		fwAvlDeleteTree(bucket->appsTree, delete_record);
    		snort_free(bucket);
    	}

    	snort_free(currBuckets);
    }
}

