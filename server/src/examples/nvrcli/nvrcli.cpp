
/************************************************************
*
* This is an example of writing a simple command line NVR
* The app is single process, which is often what one needs
* for small systems. oZone can be used to create both
* distributed systems or monolithic systems.
*
*************************************************************/
#include <iostream>
#include <thread>
#include <string>
#include <cctype>
#include <algorithm>
#include <unordered_map>
#include <map>
#include <list>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <mutex>
#include "nvrcli.h"
#include "nvrNotifyOutput.h"

#define MAX_CAMS 10
#define RECORD_VIDEO 1 // 1 if video is on
#define SHOW_FFMPEG_LOG 0
#define EVENT_REC_PATH "nvrcli_events"

#define person_resize_w 1024
#define person_resize_h 768
#define person_refresh_rate 5

#define video_record_w 640
#define video_record_h 480

using namespace std;

// Will hold all cameras and related functions
class  nvrCameras
{
public:
    AVInput *cam;
    Detector *motion; // keeping multiple detectors
    Detector *person;   
    Detector *face;   
    Recorder *event; // will either store video or images 
    RateLimiter *rate;
    LocalFileOutput *fileOut;
    bool scheduleDelete;

};

list <nvrCameras> nvrcams;
int camid=0; // id to suffix to cam-name. always increasing
Listener *listener;
NotifyOutput *notifier;
HttpController* httpController;
Application app;
Options avOptions;
std::mutex mtx;

// default URLs to use if none specified
// feel free to add custom URLs here
const char* const defRtspUrls[] = {
   "rtsp://wowzaec2demo.streamlock.net/vod/mp4:BigBuckBunny_115k.mov",
   "rtsp://wowzaec2demo.streamlock.net/vod/mp4:BigBuckBunny_115k.mov",
   "rtsp://wowzaec2demo.streamlock.net/vod/mp4:BigBuckBunny_115k.mov",
   "rtsp://wowzaec2demo.streamlock.net/vod/mp4:BigBuckBunny_115k.mov",
   "rtsp://wowzaec2demo.streamlock.net/vod/mp4:BigBuckBunny_115k.mov",
   "rtsp://wowzaec2demo.streamlock.net/vod/mp4:BigBuckBunny_115k.mov",
   "rtsp://wowzaec2demo.streamlock.net/vod/mp4:BigBuckBunny_115k.mov",
   "rtsp://wowzaec2demo.streamlock.net/vod/mp4:BigBuckBunny_115k.mov",
   "rtsp://wowzaec2demo.streamlock.net/vod/mp4:BigBuckBunny_115k.mov"
};

// traps the ultra-verbose ffmpeg logs. If you want them on enable SHOW_FFMPEG_LOG above
static void avlog_cb(void *, int level, const char * fmt, va_list vl) 
{
#if SHOW_FFMPEG_LOG
    char logbuf[2000];
    vsnprintf(logbuf, sizeof(logbuf), fmt, vl);
    logbuf[sizeof(logbuf) - 1] = '\0';
    cout  << logbuf;
#endif

}

// releases resources of a camera object
void destroyCam (nvrCameras& i)
{
    cout << "waiting for mutex lock..." << endl;
    mtx.lock();
    cout << "got mutex!" << endl;
     if (i.cam) { cout << "removing camera"<< endl;  i.cam->stop(); }
     if (i.motion) { cout<<  "stopping motion"<< endl;i.motion->deregisterAllProviders();i.motion->stop(); }
     if (i.person) { cout<<  "stopping shape detection"<< endl;i.person->deregisterAllProviders();i.person->stop(); }
     if (i.face) { cout<<  "stopping face detection"<< endl;i.face->deregisterAllProviders();i.face->stop(); }
     if (i.event) { cout << "stopping event recorder"<< endl;notifier->deregisterProvider(*(i.event)); i.event->deregisterAllProviders();i.event->stop();}
     if (i.rate) { cout << "stopping rate limiter"<< endl; i.rate->deregisterAllProviders();i.rate->stop(); }
    cout << "all done, mutex released" << endl;
    mtx.unlock();
      
}

// Adds a camera
void cmd_add()
{

    if (nvrcams.size() == MAX_CAMS)
    {
        cout << "Cannot add any more cams!\n\n";
        return;
    }

    string name;
    string source;
    string type;
    string record;

    cin.clear(); cin.sync();
    
    name = "";
    cout << "RTSP source (ENTER for default):";
    getline(cin,source);
    cout << "Detection type ([m]otion/[f]ace/[p]erson/[a]ll) (ENTER for default = a):";
    getline (cin, type);
    cout << "Record events? ([y]es/[n]o) (ENTER for default = n):";
    getline (cin, record);

    // Process input, fill in defaults if needed
    if (name.size()==0 )
    {
        string n = to_string(camid);
        camid++;
        name = "cam" + n;
    }
    cout << "Camera name:" << name << endl;
    
    if (source.size() == 0 )
    {
        source = defRtspUrls[nvrcams.size() % MAX_CAMS];
    }
    cout << "Camera source:" << source << endl;
    
    if (record.size() ==0 || (record != "y" && record != "n" ))
    {
        record = "n";
    }
    cout << "Recording will be " << (record=="n"?"skipped":"stored") << " for:" << name << endl;
    
    if (type.size() ==0 || (type != "m" && type != "f" && type != "a" && type !="p"))
    {
        type = "a";
    }


    cout << "Detection type is: ";
    if (type=="f"){cout << "Face";}
    else if (type =="m"){cout << "Motion";}
    else if (type == "p"){cout << "Person";}
    else if (type=="a"){cout << "Face+Motion+Person";}
    cout << endl; 
    
    nvrCameras nvrcam;

    // NULLify everything so I know what to delete later
    nvrcam.cam = NULL;
    nvrcam.motion = NULL;
    nvrcam.person = NULL;
    nvrcam.face = NULL;
    nvrcam.event = NULL;
    nvrcam.rate = NULL;
    nvrcam.fileOut = NULL;
    nvrcam.scheduleDelete = false;

    nvrcam.cam = new AVInput ( name, source,avOptions );
    if (type == "f") // only instantiate face recog
    {
        nvrcam.face = new FaceDetector( "person-"+name,"./shape_predictor_194_face_landmarks.dat",FaceDetector::OZ_FACE_MARKUP_OUTLINE );
        nvrcam.rate = new RateLimiter( "rate-"+name,person_refresh_rate,true );
        nvrcam.rate->registerProvider(*(nvrcam.cam) );
        nvrcam.face->registerProvider(*(nvrcam.rate),FeedLink( FEED_QUEUED, AudioVideoProvider::videoFramesOnly ) );
    }
    else if (type=="p") // only instantiate people recog
    {
        nvrcam.person = new ShapeDetector( "person-"+name,"shop.svm",ShapeDetector::OZ_SHAPE_MARKUP_OUTLINE  );
        nvrcam.rate = new RateLimiter( "rate-"+name,person_refresh_rate );
        nvrcam.rate->registerProvider(*(nvrcam.cam) );
        //nvrcam.person->registerProvider(*(nvrcam.rate),FeedLink( FEED_QUEUED, AudioVideoProvider::videoFramesOnly ) );
        nvrcam.person->registerProvider(*(nvrcam.cam),FeedLink( FEED_QUEUED, AudioVideoProvider::videoFramesOnly ) );
}
    else if (type=="m") // only instantate motion detect
    {
        nvrcam.motion = new MotionDetector( "modect-"+name );
        nvrcam.motion->registerProvider(*(nvrcam.cam) );
    }
    else // face/motion/person - turn them all on
    {
        nvrcam.person = new ShapeDetector( "person-"+name,"person.svm",ShapeDetector::OZ_SHAPE_MARKUP_OUTLINE  );
        nvrcam.face = new FaceDetector( "face-"+name, "./shape_predictor_68_face_landmarks.dat" );
        nvrcam.fileOut = new LocalFileOutput( "file-"+name, "/tmp" );
        nvrcam.rate = new RateLimiter( "rate-"+name,person_refresh_rate,true );
        nvrcam.rate->registerProvider(*(nvrcam.cam) );
        nvrcam.person->registerProvider(*(nvrcam.rate),FeedLink( FEED_QUEUED, AudioVideoProvider::videoFramesOnly ) );
        nvrcam.face->registerProvider(*(nvrcam.rate),FeedLink( FEED_QUEUED, AudioVideoProvider::videoFramesOnly ) );
        nvrcam.fileOut->registerProvider(*(nvrcam.face) );
        nvrcam.motion = new MotionDetector( "modect-"+name );
        nvrcam.motion->registerProvider(*(nvrcam.cam) );
    }

    //setup path for events recording
    char path[2000];
    snprintf (path, 1999, "%s/%s",EVENT_REC_PATH,name.c_str());
    if (record == "y")
    {
         mkdir (path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

    #if RECORD_VIDEO
        VideoParms* videoParms= new VideoParms( video_record_w, video_record_h );
        AudioParms* audioParms = new AudioParms;
        nvrcam.event = new VideoRecorder(name, path, "mp4", *videoParms, *audioParms, 30);
    #else
        nvrcam.event = new EventRecorder( "event-"+name,  path,30);
    #endif
        if (type=="m")
        { 
            nvrcam.event->registerProvider(*(nvrcam.motion));
        }
        else if (type == "f")
        {
            
            nvrcam.event->registerProvider(*(nvrcam.face));
        }
        else if (type == "p")
        {
            nvrcam.event->registerProvider(*(nvrcam.person));
        }
        else if (type == "a")
        {
            
            cout << "only registering person detection events" << endl;
            nvrcam.event->registerProvider(*(nvrcam.person));
        }
        notifier->registerProvider(*(nvrcam.event));
        notifier->start();
       cout << "Events recorded to: " << path << endl;
    }
    else 
    {
        cout << "Recording will be skipped" << endl;
    }

    nvrcams.push_back(nvrcam); // add to list
    
    cout << "Added:"<<nvrcams.back().cam->name() << endl;
    cout << nvrcams.back().cam->source() << endl;

    nvrcams.back().cam->start();
    if (nvrcams.back().motion != NULL){nvrcams.back().motion->start();}
    if (nvrcams.back().rate != NULL) {nvrcams.back().rate->start();}
    if (nvrcams.back().person != NULL) {nvrcams.back().person->start();}
    if (nvrcams.back().face != NULL) {nvrcams.back().face->start();}
    if (nvrcams.back().fileOut != NULL) {nvrcams.back().fileOut->start();}

    if (record == "y")
    {
        nvrcams.back().event->start();
    }
    listener->removeController(httpController);
    httpController->addStream("live",*(nvrcam.cam));
    if (type=="m")
    {
        httpController->addStream("debug",*(nvrcam.motion));
    }
    else if (type == "p")
    {
        httpController->addStream("debug",*(nvrcam.person));
    }
    else if (type =="f")
    {
        httpController->addStream("face",*(nvrcam.face));
    }
    else if (type == "a")
    {
        httpController->addStream("debug",*(nvrcam.motion));
        httpController->addStream("debug",*(nvrcam.person));
        httpController->addStream("debug",*(nvrcam.face));
    }
    listener->addController(httpController);
    
}

// CMD - help 
void cmd_help()
{
    cout << endl << "Possible commands: add, delete, list, stop, quit" << endl;
}

// CMD - prints a list of configured cameras
void cmd_list()
{
    int i=0;
    for (nvrCameras n:nvrcams)
    {
        cout <<i<<":"<< n.cam->name() <<"-->"<<n.cam->source() << endl;
        i++;
    }
}


// CMD - delets a camera
void cmd_delete()
{
    if (nvrcams.size() == 0)
    {
        cout << "No items to delete.\n\n";
        return;
    }
    cmd_list();
    string sx;
    unsigned int x;
    cin.clear(); cin.sync();
    do {cout << "Delete index:"; getline(cin,sx); x=stoi(sx);} while (x > nvrcams.size());
    list<nvrCameras>::iterator i = nvrcams.begin();
    while ( i != nvrcams.end()) // iterate to that index selected
    {
        if (x==0) break;
        x--;
        i++;
    }
    
    (*i).scheduleDelete = true;
}

void cmd_quit()
{
    cout << endl << "Bye."<<endl<<endl;
    exit(0);
}

// CMD - default handler
void cmd_unknown()
{
    cout << endl << "unknown command. try help"<< endl;
}


// monitors active camera list and removes them if terminated
void monitorStatus(Application app)
{
    for (;;)
    {
        list<nvrCameras>::iterator i = nvrcams.begin();
        while ( i!= nvrcams.end())
        {
            int isTerminated = (*i).cam->ended() + (*i).cam->error();
            if (isTerminated >0 || (*i).scheduleDelete == true)
            {
                cout << "Bad state found for " << (*i).cam->name() << "..deleting..."<<endl;

               
                (*i).scheduleDelete = false;
                destroyCam(*i);        
                i = nvrcams.erase(i); // point to next iterator on delete
    
            }
            else
            {
                i++; // increment if not deleted
            }
        }
        
        sleep(5);
    }
}

//  This thread will listen to commands from users
void cli(Application app)
{
    unordered_map<std::string, std::function<void()>> cmd_map;
    cmd_map["help"] = &cmd_help;
    cmd_map["add"] = &cmd_add;
    cmd_map["list"] = &cmd_list;
    cmd_map["delete"] = &cmd_delete;
    cmd_map["quit"] = &cmd_quit;
    
    
    string command;
    for (;;) 
    { 
        cin.clear(); cin.sync();
        cout << "?:";
        getline (cin,command);
        transform(command.begin(), command.end(), command.begin(), [](unsigned char c) { return tolower(c); }); // lowercase 
        cout << "You entered: "<< command << endl;
        if (cmd_map.find(command) == cmd_map.end()) 
            cmd_unknown();
        else
            cmd_map[command]();
        cout <<endl<<endl;
    }
}

int main( int argc, const char *argv[] )
{
    dbgInit( "nvrcli", "", 2 );
    cout << " \n---------------------- NVRCLI ------------------\n"
             " Type help to get started\n"
             " ------------------------------------------------\n\n";

    Info( "Starting" );

    av_log_set_callback(avlog_cb);
    avInit();
    mkdir (EVENT_REC_PATH, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    
    avOptions.add("realtime",true); // if file, honor fps, don't blaze through
    avOptions.add("loop",true); // if file, loop on end

    listener = new Listener;
    httpController = new HttpController( "watch", 9292 );
    listener->addController( httpController );
    app.addThread( listener );

    notifier = new NotifyOutput("notifier");
    app.addThread(notifier);

    thread t1(cli,app);
    thread t2(monitorStatus,app);
    
    app.run();
    cout << "Never here";
}
