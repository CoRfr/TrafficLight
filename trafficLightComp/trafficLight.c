#include "legato.h"
#include "le_cfg_interface.h"
#include "interfaces.h"
#include <curl/curl.h>

#define SSL_ERROR_HELP    "Make sure your system date is set correctly (e.g. `date -s '2016-7-7'`)"
#define SSL_ERROR_HELP_2  "Check the minimum date for this SSL cert to work"
#define MIN(a,b) (((a)<(b))?(a):(b))
#define ARRAY_SIZE 512

// Url that is settable through config tree
static char Url[ARRAY_SIZE] = "";

// Polling timer interval in seconds
static int PollingIntervalSec = 10;

// Memory pool and timer reference
static le_mem_PoolRef_t PoolRef;
static le_timer_Ref_t PollingTimer = NULL;

// Header declaration
static void GpioInit(void);
static void Polling(le_timer_Ref_t timerRef);
static void TimerHandle();

//--------------------------------------------------------------------------------------------------
/**
 * Holds information of the htmlstring from url
 */
//--------------------------------------------------------------------------------------------------
typedef struct
{
    char* actualData;
}
MemoryPool_t;

//--------------------------------------------------------------------------------------------------
/**
 * Light statuses
 *
 * @note Enumerated type is used in SetLightState function
 */
//--------------------------------------------------------------------------------------------------
typedef enum
{
    LIGHT_RED,
    LIGHT_YELLOW,
    LIGHT_GREEN,
    LIGHT_OFF,
}
LightState_t;

//--------------------------------------------------------------------------------------------------
/**
 * Monitor statuses used for comparison to ultimately set the lightstates
 */
//--------------------------------------------------------------------------------------------------
typedef enum
{
    STATE_FAIL,
    STATE_WARNING,
    STATE_PASS,
}
MonitorState_t;

//--------------------------------------------------------------------------------------------------
/**
 * Sets the GPIO pins to active_high with respect to the light states.
 *
 * @return
 *      GPIO pins
 */
//--------------------------------------------------------------------------------------------------
static void SetLightState
(
    LightState_t state    ///< [IN] States of the lights.
)
{
    bool gpioRed = false;
    bool gpioYellow = false;
    bool gpioGreen = false;

    switch(state)
    {
        case LIGHT_GREEN:
            gpioGreen = true;
            break;
        case LIGHT_YELLOW:
            gpioYellow = true;
            break;
        case LIGHT_RED:
            gpioRed = true;
            break;
        case LIGHT_OFF:
            break;
        default:
            break;
    }

    le_gpioGreen_SetPushPullOutput(LE_GPIOGREEN_ACTIVE_HIGH, gpioGreen);
    le_gpioYellow_SetPushPullOutput(LE_GPIOYELLOW_ACTIVE_HIGH, gpioYellow);
    le_gpioRed_SetPushPullOutput(LE_GPIORED_ACTIVE_HIGH, gpioRed);
}

//--------------------------------------------------------------------------------------------------
/**
 * 1. Takes the data from bufferPtr and allocates memory to store in userDataPtr.
 *
 * 2. The bufferPtr data holds the HTML text.
 *
 * @return
 *      size and information of the data that was received
 */
//--------------------------------------------------------------------------------------------------
static size_t WriteCallback
(
    void *bufferPtr,      ///< [IN] Ptr to the string of information.
    size_t size,          ///< [IN] size of individual elements
    size_t nbMember,      ///< [IN] number of elements in bufferPtr
    void *userDataPtr     ///< [OUT] memory containing information and size
)
{
    size_t realsize = size * nbMember;
    MemoryPool_t * memoryPoolPtr = (MemoryPool_t *) userDataPtr;

    LE_DEBUG("bufferPtr = %s. This should match myPool.actualData in the GetUrl function", (char *) bufferPtr);

    memoryPoolPtr->actualData = (char*) bufferPtr;

    return realsize;
}

//--------------------------------------------------------------------------------------------------
/**
 * Check the state of a Jenkins job based on the content by the REST API <job>/<build>/api/xml
 *
 * @return
 *      light states
 *      contentResult in config tree
 */
//--------------------------------------------------------------------------------------------------
static MonitorState_t CheckJenkinsResult
(
    char * actualData     ///< [IN] Data that was handled in GetUrl with WriteCallback
)
{
    char * contentResult;
    MonitorState_t status;

    if( strstr(actualData, "SUCCESS") )
    {
        contentResult = "SUCCESS";
        status = STATE_PASS;
    }
    else if( strstr(actualData, "FAILURE") )
    {
        contentResult = "FAILURE";
        status = STATE_FAIL;
    }
    else if( strstr(actualData, "ABORTED") )
    {
        contentResult = "ABORTED";
        status = STATE_WARNING;
    }
    else if( strstr(actualData, "UNSTABLE") )
    {
        contentResult = "UNSTABLE";
        status = STATE_WARNING;
    }
    else
    {
        contentResult = "NULL";
        LE_ERROR("Cannot find keyword for statuses");
        status = STATE_FAIL;
    }

    LE_INFO("contentResult = %s", contentResult);

    return status;
}

//--------------------------------------------------------------------------------------------------
/**
 * Takes a char array and given a character value, return the index of the array with the
 * first occurence of the value.
 */
//--------------------------------------------------------------------------------------------------
static int GetIndexOfArrayValue
(
    char * string,
    size_t size,
    char value
)
{
    int index = 0;

    while ( index < size && string[index] != value ) ++index;

    return ( index == size ? -1 : index );
}

//--------------------------------------------------------------------------------------------------
/**
 * Check the Sensu state based on content as returned by the '/metrics' REST API.
 *
 * @return
 *      light states
 *      contentResult in config tree
 */
//--------------------------------------------------------------------------------------------------
static MonitorState_t CheckSensuResult
(
    char * actualData     ///< [IN] Data that was handled in GetUrl with WriteCallback
)
{
    int index = 0;
    size_t length;

    length = strlen(actualData);

    while( index != -1 )
    {
        index = MIN(GetIndexOfArrayValue(actualData, length, 'c'), GetIndexOfArrayValue(actualData, length, 'w'));

        // Truncate string until the string starts where 'c' is
        memmove(actualData, actualData + index, length - index + 1);
        length = strlen(actualData);

        // Check if the keyword is actually critical and find if there is a failure on the key
        if( !strncmp(actualData, "critical", 8) )
        {
            // Looks to see if the key to the mapping of "critical" is 0. If not, then there is an error
            if( actualData[10] != '0')
            {
                LE_ERROR("There are %c machines in critical state", actualData[10]);
                return STATE_FAIL;
            }
        }
        // Check if the keyword is actually warning and find if there is a failure on the key
        if( !strncmp(actualData, "warning", 7) )
        {
            // Looks to see if the key to the mapping of "warning" is 0. If not, then there is an error
            if( actualData[9] != '0')
            {
                LE_ERROR("There are %c machines in warning state", actualData[9]);
                return STATE_WARNING;
            }
        }

        memmove(actualData, actualData + 1, length);
        length = strlen(actualData);
    }

    return STATE_PASS;
}

//--------------------------------------------------------------------------------------------------
/**
 * Gets the HTTP code status of the Url every x seconds only if exitCodeCheck flag is true
 *
 * @return
 *      HTTP code
 */
//--------------------------------------------------------------------------------------------------
static MonitorState_t GetHTTPCode
(
    CURL *curlPtr                     ///< [IN] curlPtr handle to perform curl functions
)
{
    int httpCode;
    MonitorState_t status;

    curl_easy_getinfo(curlPtr, CURLINFO_RESPONSE_CODE, &httpCode);

    LE_INFO("exitCodeExpected (httpCode): %i", httpCode);

    if(httpCode == 200)
    {
        status = STATE_PASS;
    }
    else
    {
        status = STATE_FAIL;
    }

    return status;
}

//--------------------------------------------------------------------------------------------------
/**
 * 1. Called by Polling function
 *
 * 2. Tries to check the Url that is set in the config tree and store content into buffer
 *
 * 3. Calls functions to display light depending on the boolean values of exitCodeCheck and contentCheck
 */
//--------------------------------------------------------------------------------------------------
static void GetUrl
(
    void
)
{
    CURL *curlPtr;                          ///<- Easy handle necessary for curl functions
    CURLcode res;                           ///<- Stores results of curl functions

    MemoryPool_t myPool;

    MonitorState_t exitCodeState = STATE_PASS;
    MonitorState_t contentState = STATE_PASS;

    // Get Url from config Tree
    le_cfg_QuickGetString("/url", Url, sizeof(Url), Url);

    LE_INFO("Url: %s", Url);

    // Curl Operations
    curlPtr = curl_easy_init();
    if (curlPtr)
    {
        curl_easy_setopt(curlPtr, CURLOPT_URL, Url);

        //Write data into actualData. The conditionals check for errors
        res = curl_easy_setopt(curlPtr, CURLOPT_WRITEFUNCTION, WriteCallback);
        if( res == CURLE_WRITE_ERROR)
        {
            LE_ERROR("curlopt_writefunction failed: %s", curl_easy_strerror(res));
        }

        // Create a memory pool to store the htmlString. If exists, do not create anymore duplicates
        if( !le_mem_FindPool("htmlString") )
        {
            LE_DEBUG("Created local memory pool 'htmlString'");
            PoolRef = le_mem_CreatePool("htmlString", sizeof(MemoryPool_t));
        }
        myPool.actualData = le_mem_ForceAlloc(PoolRef);

        res = curl_easy_setopt(curlPtr, CURLOPT_WRITEDATA, (void *) &myPool);
        if( res != CURLE_OK)
        {
            LE_ERROR("curlopt_writedata failed: %s", curl_easy_strerror(res));

            SetLightState(STATE_WARNING);
        }

        res = curl_easy_perform(curlPtr);
        if (res != CURLE_OK)
        {
            LE_ERROR("curl_easy_perform() failed: %s", curl_easy_strerror(res));
            if (res == CURLE_SSL_CACERT)
            {
                LE_ERROR(SSL_ERROR_HELP);
                LE_ERROR(SSL_ERROR_HELP_2);
            }

            SetLightState(STATE_WARNING);
        }
        else
        {
            // States are described in README.md
            if(le_cfg_QuickGetBool("/info/exitCode/checkFlag", false))
            {
                exitCodeState = GetHTTPCode(curlPtr);
            }

            if(le_cfg_QuickGetBool("/info/content/checkFlag", false))
            {
                if( myPool.actualData[strlen(myPool.actualData)] != '\0')
                {
                    contentState = STATE_FAIL;
                    LE_ERROR("There is no NULL char at the end of the buffer");
                }
                else
                {
                    char checkMode[256] = {0};

                    le_cfg_QuickGetString("/info/content/checkMode",
                                          checkMode,
                                          sizeof(checkMode),
                                          "");

                    if(strncmp(checkMode, "sensu", sizeof(checkMode)) == 0)
                    {
                        contentState = CheckSensuResult(myPool.actualData);
                    }
                    else if(strncmp(checkMode, "jenkins", sizeof(checkMode)) == 0)
                    {
                        contentState = CheckJenkinsResult(myPool.actualData);
                    }
                    else{
                        LE_ERROR("Not checking Sensu-client or jenkins job");
                    }
                }
            }

            SetLightState( MIN(exitCodeState, contentState) );
        }

        curl_easy_cleanup(curlPtr);
    }
    else
    {
        LE_ERROR("Couldn't initialize cURL.");
    }

    curl_global_cleanup();
}


//--------------------------------------------------------------------------------------------------
/**
 * Initializes IoT pins 13, 15, 17 (green, yellow, red respectively) for output
 *
 * @return
 *      Activated pins and initialize light to be off
 */
//--------------------------------------------------------------------------------------------------
static void GpioInit
(
    void
)
{
    le_gpioRed_Activate();
    le_gpioRed_EnablePullUp();

    le_gpioYellow_Activate();
    le_gpioYellow_EnablePullUp();

    le_gpioGreen_Activate();
    le_gpioGreen_EnablePullUp();

    SetLightState(LIGHT_OFF);

    LE_DEBUG("RED read PP - High: %d", le_gpioRed_Read());
    LE_DEBUG("YELLOW read PP - High: %d", le_gpioYellow_Read());
    LE_DEBUG("GREEN read PP - High: %d", le_gpioGreen_Read());
}

//--------------------------------------------------------------------------------------------------
/**
 * 1. Initialize the indefinitely repeating PollingTimer to x seconds
 *
 * 2. Reset polling interval from Polling function
 *
 * @return
 *      A timer reference PollingTimer
 */
//--------------------------------------------------------------------------------------------------
static void TimerHandle
(
    void
)
{
    if(le_timer_IsRunning(PollingTimer))
    {
        le_timer_Stop(PollingTimer);
    }
    LE_DEBUG("pollingIntervalSec is %i", PollingIntervalSec);
    le_clk_Time_t interval = {PollingIntervalSec, 0}; // first parameter is the seconds
    le_timer_SetInterval(PollingTimer, interval);
    le_timer_SetRepeat(PollingTimer, 0); // repeat indefinitely
    le_timer_SetHandler(PollingTimer, Polling);
    le_timer_Start(PollingTimer);
}

//--------------------------------------------------------------------------------------------------
/**
 * This function is called every x seconds where x can be set in the configTree.
 *
 * eg. config set trafficLight:/pollingIntervalSec 10 int
 */
//--------------------------------------------------------------------------------------------------
static void Polling
(
    le_timer_Ref_t timerRef      ///< [IN] timer reference for changing intervals, starting and stopping
)
{
    int timerSet;

    timerSet = le_cfg_QuickGetInt("/pollingIntervalSec", PollingIntervalSec);

    if(timerSet != PollingIntervalSec)
    {
        PollingIntervalSec = timerSet;
        TimerHandle();
    }

    LE_INFO("-------------------------- In polling function--------------------");

    GetUrl();
}

//--------------------------------------------------------------------------------------------------
/**
 * Deactivate any active pins that are set currently so that the GPIO pins are usable
 */
//--------------------------------------------------------------------------------------------------
static void GpioDeinit
(
    void
)
{
    le_gpioGreen_Deactivate();
    le_gpioYellow_Deactivate();
    le_gpioRed_Deactivate();
}

//--------------------------------------------------------------------------------------------------
/**
 * Handles internal states of GPIO pins when the app is terminated by the user.
 *
 * @return
 *      Deactivated GPIO pins
 */
//--------------------------------------------------------------------------------------------------
static void SigTermEventHandler
(
    int sigNum
)
{
    le_timer_Stop(PollingTimer);
    LE_INFO("Deactivating GPIO Pins");
    GpioDeinit();
}

//---------------------------------------------------
/**
 * Initializes GPIO Pins, and ConfigTree
 */
//---------------------------------------------------
COMPONENT_INIT
{
    const char * wakeUpTag = "trafficLightWakeUpTag";
    le_pm_WakeupSourceRef_t wakeUpRef = le_pm_NewWakeupSource(1, wakeUpTag);
    le_pm_StayAwake(wakeUpRef);

    curl_global_init(CURL_GLOBAL_ALL);
    GpioInit();

    PollingTimer = le_timer_Create("PollingTimer");
    TimerHandle();

    le_sig_Block(SIGTERM);
    le_sig_SetEventHandler(SIGTERM, SigTermEventHandler);
}
