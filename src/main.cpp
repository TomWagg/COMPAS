//
//  COMPAS main
//
#include <ctime>
#include <chrono>
#include <string>
#include <sstream>
#include <fstream>
#include <tuple>
#include <vector>
#include <csignal>

#include "constants.h"
#include "typedefs.h"

#include "profiling.h"
#include "utils.h"
#include "Options.h"
#include "Rand.h"
#include "Log.h"

#include "AIS.h"
#include "Star.h"
#include "BinaryStar.h"

OBJECT_ID globalObjectId = 1;                                   // used to uniquely identify objects - used primarily for error printing
OBJECT_ID m_ObjectId     = 0;                                   // object id for main - always 0


class AIS;
class Star;
class BinaryStar;

OBJECT_ID    ObjectId()    { return m_ObjectId; }
OBJECT_TYPE  ObjectType()  { return OBJECT_TYPE::MAIN; }
STELLAR_TYPE StellarType() { return STELLAR_TYPE::NONE; }


// The following global variables support the BSE Switch Log file
// Ideally, rather than be declared as globals, they would be in maybe the 
// LOGGING service singleton, but the Log class knows nothing about the 
// BinaryStar class...
// (maybe we could put them in the new CONSTANTS service singleton if we 
// implement it)

BinaryStar* evolvingBinaryStar      = NULL;             // pointer to the currently evolving Binary Star
bool        evolvingBinaryStarValid = false;            // flag to indicate whether the evolvingBinaryStar pointer is valid

/*
 * Signal handler
 * 
 * Only handles SIGUSR1; all other signals are left to the system to handle.
 * 
 * SIGUSR1 is a user generated signal - the system should not generate this signal,
 * though it is possible to send the signal to a process via the Un*x kill command,
 * or some other user-developed program that sends signals.  This code does some 
 * rudimentary sanity checks, but it is possible that sending a SIGUSR1 signal to a
 * running COMPAS process via the Un*x kill command, or otherwise, might cause a 
 * spurious entry in the BSE Switch Log file - c'est la vie.
 * 
 * We use SIGUSR1 in the Star class to signal when a Star object switches stellar 
 * type. We use a signal because the Star class knows nothing about binary stars, 
 * so can't call a binary star function to log binary star variables to the BSE 
 * Switch Log file. By raising a signal in the Star class and catching it here we 
 * can call the appropriate binary star class function to write the binary star 
 * variables to the log file.
 * 
 * The signal is raised in the Star::SwitchTo() function if OPTIONS->BSESwitchLog() 
 * is true, so the signal will be received here for every stellar type switch of 
 * every star.
 * 
 * We only process the signal here if the global variable evolvingBinaryStarValid 
 * is true. The global variable evolvingBinaryStarValid is only set true after a 
 * binary star has been constructed and is ready to evolve - so if the signal is 
 * raised, it will be ignored for SSE switches, and it will be ignored for switches 
 * inside the constructor of the binary star (and so its constituent stars).
 * 
 * This signal handler is installed in EvolveBinaryStars(), so it is installed only
 * if we're evolving binaries - signals will be ignored (by our code - the system
 * will still receive and handle them) if we're evolving single stars.
 *
 * 
 * void sigHandler(int p_Sig)
 * 
 * @param   [IN]        p_Sig                   The signal intercepted
 * 
 */
void sigHandler(int p_Sig) {   
    if (p_Sig == SIGUSR1) {                                         // SIGUSR1?  Just silently ignore anything else...
        if (evolvingBinaryStarValid && OPTIONS->BSESwitchLog()) {   // yes - do we have a valid binary star, and are we logging BSE switches?
            evolvingBinaryStar->PrintSwitchLog();                   // yes - assume SIGUSR1 is a binary constituent star switching...
        }
    }
}


/*
 * Evolve single stars
 *
 *
 * std::tuple<int, int> EvolveSingleStars()
 * 
 * @return                                      Tuple: <number of stars requested, actual number of stars created>
 */
std::tuple<int, int> EvolveSingleStars() {

    EVOLUTION_STATUS evolutionStatus = EVOLUTION_STATUS::CONTINUE;

    auto wallStart = std::chrono::system_clock::now();                                                                  // start wall timer
    clock_t clockStart = clock();                                                                                       // start CPU timer

    std::time_t timeStart = std::chrono::system_clock::to_time_t(wallStart);
    SAY("Start generating stars at " << std::ctime(&timeStart));

    double massInc = (OPTIONS->SingleStarMassMax() - OPTIONS->SingleStarMassMin()) / OPTIONS->SingleStarMassSteps();    // use user-specified values if no grid file

    // generate and evolve stars

    bool usingGrid     = !OPTIONS->GridFilename().empty();                                                              // using grid file?
    int  nStars        = usingGrid ? 1 : OPTIONS->SingleStarMassSteps();                                                // how many stars? (grid file is 1 per record...)
    int  nStarsCreated = 0;                                                                                             // number of stars actually created
    int  index         = 0;                                                                                             // which star

    Star* star = nullptr;
    while (evolutionStatus == EVOLUTION_STATUS::CONTINUE && index < nStars) {                                           // for each star to be evolved

        double initialMass = 0.0;                                                                                       // initial mass of star

        if (usingGrid) {                                                                                                // using grid file?
            int gridResult = OPTIONS->ApplyNextGridRecord();                                                            // yes - set options according to specified values in grid file              
            switch (gridResult) {                                                                                       // handle result of grid file read
                case -1: evolutionStatus = EVOLUTION_STATUS::STOPPED; break;                                            // read error - stop evolution
                case  0: evolutionStatus = EVOLUTION_STATUS::DONE; break;                                               // nothing to read - we're done

                case  1:                                                                                                // grid record read - not done yet...
                    nStars++;                                                                                           // increment count of stars requested to be evolved
                    initialMass = OPTIONS->Mass();                                                                      // set initial mass for the star being evolved
                    break;             

                default: evolutionStatus = EVOLUTION_STATUS::STOPPED; break;                                            // problem - stop evolution
            }
        }
        else {                                                                                                          // no, not using a grid file
            double initialMass = OPTIONS->SingleStarMassMin() + (index * massInc);                                      // calculate the initial mass for the next star
        }

        if (evolutionStatus == EVOLUTION_STATUS::CONTINUE) {                                                            // ok?

            // Single stars (in SSE) are provided with a random seed that is used
            // to seed the random number generator.  The random number generator
            // is re-seeded for each star. Here we generate the seed for the star
            // being evolved - by this point we have picked up the option value
            // from either the commandline or the grid file.
            //
            // If OPTIONS->FixedRandomSeed() is true the user specified a random seed 
            // via the program option --random-seed.  The random seed specified by the
            // user is the base random seed - the actual random seed used for each star
            // (in SSE) is the base random seed (specified by the user) plus the index
            // of the star being evolved.  The index of the star being evolved starts at
            // 0 for the first star, and increments by 1 for each subsequent star evolved
            // (so the base random seed specified by the user is also the initial random 
            // seed - the random seed of the first star evolved)

            unsigned long int randomSeed = 0l;
            if (OPTIONS->FixedRandomSeed()) {                                                                           // user supplied seed for the random number generator?
                randomSeed = RAND->Seed(OPTIONS->RandomSeed() + (long int)index);                                       // yes - this allows the user to reproduce results for each star
            }
            else {                                                                                                      // no
                randomSeed = RAND->Seed(RAND->DefaultSeed() + (long int)index);                                         // use default seed (based on system time) + star id
            }

            // Single stars (in SSE) are provided with a kick structure that specifies the 
            // values of the random number to be used to generate to kick magnitude, and the
            // actual kick magnitude specified by the user via program option --kick-magnitude       
            //
            // See typedefs.h for the kick structure.
            //
            // We can't just pick up the values of the options inside Basestar.cpp because the
            // constituents of binaries get different values, so use different options. The
            // Basestar.cpp code doesn't know if the star is a single star (SSE) or a constituent
            // of a binary (BSE) - it only knows that it is a star - so we have to setup the kick
            // structure here (and in EvolveBinaryStars() for binaries).
            //
            // for SSE only need magnitudeRandom and magnitude - other values can just be ignored

            KickParameters kickParameters;
            kickParameters.magnitudeRandomSpecied = OPTIONS->OptionSpecified("kick-magnitude-random");
            kickParameters.magnitudeRandom        = OPTIONS->KickMagnitudeRandom();
            kickParameters.magnitudeSpecied       = OPTIONS->OptionSpecified("kick-magnitude");
            kickParameters.magnitude              = OPTIONS->KickMagnitude();
                       
            // create the star
            delete star;                                                                                                // so we don't leak...
            star = new Star(randomSeed, initialMass, kickParameters);                                                   // create star according to the user-specified options

            // evolve the star
            star->Evolve(index);

            // announce results if required
            if (!OPTIONS->Quiet()) {                                                                                    // quiet mode?
                SAY(index               <<                                                                              // no - announce result of evolving the star
                    ": RandomSeed = "   <<
                    randomSeed          <<
                    ", Initial Mass = " <<
                    initialMass         <<
                    ", Metallicity = "  <<
                    star->Metallicity() <<
                    ", "                <<
                    STELLAR_TYPE_LABEL.at(star->StellarType()));
            }
            nStarsCreated++;                                                                                            // increment the number of stars created
        }

        if (!LOGGING->CloseStandardFile(LOGFILE::SSE_PARAMETERS)) {                                                     // close single star output file
            SHOW_WARN(ERROR::FILE_NOT_CLOSED);                                                                          // close failed - show warning
            evolutionStatus = EVOLUTION_STATUS::STOPPED;                                                                // this will cause problems later - stop evolution
        }

        if (!LOGGING->CloseStandardFile(LOGFILE::SSE_SWITCH_LOG)) {                                                     // close SSE switch log file if necessary
            SHOW_WARN(ERROR::FILE_NOT_CLOSED);                                                                          // close failed - show warning
            evolutionStatus = EVOLUTION_STATUS::STOPPED;                                                                // this will cause problems later - stop evolution
        }
        ERRORS->Clean();                                                                                                // clean the dynamic error catalog

        index++;                                                                                                        // next...
    }

    // all stars done - tidy up
    delete star;                                                                                                        // clean up that last star...

    if (evolutionStatus == EVOLUTION_STATUS::CONTINUE && index >= nStars) evolutionStatus = EVOLUTION_STATUS::DONE;     // set done

    int nStarsRequested = !usingGrid ? OPTIONS->SingleStarMassSteps() : (evolutionStatus == EVOLUTION_STATUS::DONE ? nStarsCreated : -1);

    SAY("\nGenerated " << std::to_string(nStarsCreated) << " of " << (nStarsRequested < 0 ? "<INCOMPLETE GRID>" : std::to_string(nStarsRequested)) << " stars requested");

    // announce result
    if (!OPTIONS->Quiet()) {
        if (evolutionStatus != EVOLUTION_STATUS::CONTINUE) {                                                            // shouldn't be
            SAY("\n" << EVOLUTION_STATUS_LABEL.at(evolutionStatus));
        }
        else {
            SHOW_WARN(ERROR::STELLAR_SIMULATION_STOPPED, EVOLUTION_STATUS_LABEL.at(EVOLUTION_STATUS::ERROR));           // show warning
        }
    }

    // close SSE logfiles
    // don't check result here - let log system handle it
    (void)LOGGING->CloseAllStandardFiles();                                                                             // close any standard log files

    // announce timing stats
    double cpuSeconds = (clock() - clockStart) / (double) CLOCKS_PER_SEC;                                               // stop CPU timer and calculate seconds

    auto wallEnd = std::chrono::system_clock::now();                                                                    // stop wall timer
    std::time_t timeEnd = std::chrono::system_clock::to_time_t(wallEnd);                                                // get end time and date

    SAY("\nEnd generating stars at " << std::ctime(&timeEnd));
    SAY("Clock time = " << cpuSeconds << " CPU seconds");

    std::chrono::duration<double> wallSeconds = wallEnd - wallStart;                                                    // elapsed seconds

    int wallHH = (int)(wallSeconds.count() / 3600.0);                                                                   // hours
    int wallMM = (int)((wallSeconds.count() - ((double)wallHH * 3600.0)) / 60.0);                                       // minutes
    int wallSS = (int)(wallSeconds.count() - ((double)wallHH * 3600.0) - ((double)wallMM * 60.0));                      // seconds

    SAY("Wall time  = " << wallHH << ":" << wallMM << ":" << wallSS << " (hh:mm:ss)");

    return  std::make_tuple(nStarsRequested, nStarsCreated);
}


/*
 * Evolve binary stars
 *
 *
 * std::tuple<int, int> EvolveBinaryStars()
 * 
 * @return                                      Tuple: <number of binaries requested, actual number of binaries created>
 */
std::tuple<int, int> EvolveBinaryStars() {

    signal(SIGUSR1, sigHandler);                                                                                        // install signal handler

    EVOLUTION_STATUS evolutionStatus = EVOLUTION_STATUS::CONTINUE;

    auto wallStart = std::chrono::system_clock::now();                                                                  // start wall timer
    clock_t clockStart = clock();                                                                                       // start CPU timer

    std::time_t timeStart = std::chrono::system_clock::to_time_t(wallStart);
    SAY("Start generating binaries at " << std::ctime(&timeStart));

    AIS ais;                                                                                                            // Adaptive Importance Sampling (AIS)

    if (OPTIONS->AIS_ExploratoryPhase()) ais.PrintExploratorySettings();                                                // print the selected options for AIS Exploratory phase in the beginning of the run
    if (OPTIONS->AIS_RefinementPhase() ) ais.DefineGaussians();                                                         // if we are sampling using AIS (step 2):read in gaussians

    // generate and evolve binaries

    bool usingGrid = !OPTIONS->GridFilename().empty();                                                                  // using grid file?
    int  nBinaries = usingGrid ? 1 : OPTIONS->nBinaries();                                                              // how many binaries? (grid file is 1 per record...)
    int  nBinariesCreated = 0;                                                                                          // number of binaries actually created
    int  index     = 0;                                                                                                 // which binary

    BinaryStar *binary = nullptr;
    while (evolutionStatus == EVOLUTION_STATUS::CONTINUE && index < nBinaries) {                                        // for each binary to be evolved

        evolvingBinaryStar      = NULL;                                                                                 // unset global pointer to evolving binary (for BSE Switch Log)
        evolvingBinaryStarValid = false;                                                                                // indicate that the global pointer is not (yet) valid (for BSE Switch log)

        if (usingGrid) {                                                                                                // using grid file?
            int gridResult = OPTIONS->ApplyNextGridRecord();                                                            // yes - set options according to specified values in grid file              
            switch (gridResult) {                                                                                       // handle result of grid file read
                case -1: evolutionStatus = EVOLUTION_STATUS::STOPPED; break;                                            // read error - stop evolution
                case  0: evolutionStatus = EVOLUTION_STATUS::DONE; break;                                               // nothing to read - we're done

                case  1:                                                                                                // grid record read - not done yet...
                    nBinaries++;                                                                                        // increment count of binaries requested to be evolved
                    break;             

                default: evolutionStatus = EVOLUTION_STATUS::STOPPED; break;                                            // problem - stop evolution
            }
        }
        else {                                                                                                          // no, not using a grid file

        DRAW MASSES HERE??? NO - LEAVE IT FOR BASEBINARYSTAR - IT KNOWS LL THE DEPENDENCIES
            double initialMass = OPTIONS->SingleStarMassMin() + (index * massInc);                                      // calculate the initial mass for the next star
        }

        if (evolutionStatus == EVOLUTION_STATUS::CONTINUE) {                                                            // ok?

            // Single stars (in SSE) are provided with a random seed that is used
            // to seed the random number generator.  The random number generator
            // is re-seeded for each star. Here we generate the seed for the star
            // being evolved - by this point we have picked up the option value
            // from either the commandline or the grid file.
            //
            // If OPTIONS->FixedRandomSeed() is true the user specified a random seed 
            // via the program option --random-seed.  The random seed specified by the
            // user is the base random seed - the actual random seed used for each star
            // (in SSE) is the base random seed (specified by the user) plus the index
            // of the star being evolved.  The index of the star being evolved starts at
            // 0 for the first star, and increments by 1 for each subsequent star evolved
            // (so the base random seed specified by the user is also the initial random 
            // seed - the random seed of the first star evolved)

            unsigned long int randomSeed = 0l;
            if (OPTIONS->FixedRandomSeed()) {                                                                           // user supplied seed for the random number generator?
                randomSeed = RAND->Seed(OPTIONS->RandomSeed() + (long int)index);                                       // yes - this allows the user to reproduce results for each star
            }
            else {                                                                                                      // no
                randomSeed = RAND->Seed(RAND->DefaultSeed() + (long int)index);                                         // use default seed (based on system time) + star id
            }

            // Single stars (in SSE) are provided with a kick structure that specifies the 
            // values of the random number to be used to generate to kick magnitude, and the
            // actual kick magnitude specified by the user via program option --kick-magnitude       
            //
            // See typedefs.h for the kick structure.
            //
            // We can't just pick up the values of the options inside Basestar.cpp because the
            // constituents of binaries get different values, so use different options. The
            // Basestar.cpp code doesn't know if the star is a single star (SSE) or a constituent
            // of a binary (BSE) - it only knows that it is a star - so we have to setup the kick
            // structure here (and in EvolveBinaryStars() for binaries).
            //
            // for SSE only need magnitudeRandom and magnitude - other values can just be ignored

            KickParameters kickParameters;
            kickParameters.magnitudeRandomSpecied = (OPTIONS->OptionSpecified("kick-magnitude-random") == 1);  // CHECK BAD OPTION HERE>???  RETURN = -1????
            kickParameters.magnitudeRandom        = OPTIONS->KickMagnitudeRandom();
            kickParameters.magnitudeSpecied       = OPTIONS->OptionSpecified("kick-magnitude");
            kickParameters.magnitude              = OPTIONS->KickMagnitude();
                       





    EVOLUTION_STATUS evolutionStatus = EVOLUTION_STATUS::CONTINUE;

    int nBinariesCreated = 0;                                                                                               // number of binaries actually created

    auto wallStart = std::chrono::system_clock::now();                                                                      // start wall timer
    clock_t clockStart = clock();                                                                                           // start CPU timer

    if (!OPTIONS->Quiet()) {
        std::time_t timeStart = std::chrono::system_clock::to_time_t(wallStart);
        SAY("Start generating binaries at " << std::ctime(&timeStart));
    }

    AIS ais;                                                                                                                // Adaptive Importance Sampling (AIS)

    if (OPTIONS->AIS_ExploratoryPhase()) ais.PrintExploratorySettings();                                                    // print the selected options for AIS Exploratory phase in the beginning of the run
    if (OPTIONS->AIS_RefinementPhase() ) ais.DefineGaussians();                                                             // if we are sampling using AIS (step 2):read in gaussians

    // generate and evolve binaries

    bool usingGrid = !OPTIONS->GridFilename().empty();                                                                      // using grid file?
    int  nBinaries = usingGrid ? 1 : OPTIONS->nBinaries();                                                                  // how many binaries? (grid file is 1 per record...)
    int  index     = 0;                                                                                                     // which binary

    BinaryStar *binary = nullptr;
    while (evolutionStatus == EVOLUTION_STATUS::CONTINUE && index < nBinaries) {

        evolvingBinaryStar      = NULL;                                                                                     // unset global pointer to evolving binary (for BSE Switch Log)
        evolvingBinaryStarValid = false;                                                                                    // indicate that the global pointer is not (yet) valid (for BSE Switch log)

        if (usingGrid) {                                                                                                    // using grid file?
            int gridResult = OPTIONS->ApplyNextGridRecord();                                                                // yes - set options according to specified values in grid file              
            switch (gridResult) {                                                                                           // handle result of grid file read
                case -1: evolutionStatus = EVOLUTION_STATUS::STOPPED; break;                                                // read error - stop evolution
                case  0: evolutionStatus = EVOLUTION_STATUS::DONE; break;                                                   // nothing to read - we're done
                case  1: nBinaries++; break;                                                                                // grid record read - not done yet...             
                default: evolutionStatus = EVOLUTION_STATUS::STOPPED; break;                                                // problem - stop evolution
            }
        }

        if (evolutionStatus == EVOLUTION_STATUS::CONTINUE) {                                                                // ok?
       
            delete binary;
            binary = new BinaryStar(ais, (long int)index);                                                                  // generate binary according to the user options

            evolvingBinaryStar      = binary;                                                                               // set global pointer to evolving binary (for BSE Switch Log)
            evolvingBinaryStarValid = true;                                                                                 // indicate that the global pointer is now valid (for BSE Switch Log)

            EVOLUTION_STATUS binaryStatus = binary->Evolve();                                                               // evolve the binary

            // announce result of evolving the binary
            if (!OPTIONS->Quiet()) {
                if (OPTIONS->CHE_Option() == CHE_OPTION::NONE) {
                    SAY(index                                      << ": "  <<
                        EVOLUTION_STATUS_LABEL.at(binaryStatus)    << ": "  <<
                        STELLAR_TYPE_LABEL.at(binary->Star1Type()) << " + " <<
                        STELLAR_TYPE_LABEL.at(binary->Star2Type())
                    );
                }
                else {
                    SAY(index                                             << ": "    <<
                        EVOLUTION_STATUS_LABEL.at(binaryStatus)           << ": ("   <<
                        STELLAR_TYPE_LABEL.at(binary->Star1InitialType()) << " -> "  <<
                        STELLAR_TYPE_LABEL.at(binary->Star1Type())        << ") + (" <<
                        STELLAR_TYPE_LABEL.at(binary->Star2InitialType()) << " -> "  <<
                        STELLAR_TYPE_LABEL.at(binary->Star2Type())        <<  ")"
                    );
                }
            }

            nBinariesCreated++;                                                                                             // increment the number of binaries created

            if (OPTIONS->AIS_ExploratoryPhase() && ais.ShouldStopExploratoryPhase(index)) {                                 // AIS says should stop simulation?
                evolutionStatus = EVOLUTION_STATUS::AIS_EXPLORATORY;                                                        // ... and stop
            }

            if (!LOGGING->CloseStandardFile(LOGFILE::BSE_DETAILED_OUTPUT)) {                                                // close detailed output file if necessary
                SHOW_WARN(ERROR::FILE_NOT_CLOSED);                                                                          // close failed - show warning
                evolutionStatus = EVOLUTION_STATUS::STOPPED;                                                                // this will cause problems later - stop evolution
            }

            if (!LOGGING->CloseStandardFile(LOGFILE::BSE_SWITCH_LOG)) {                                                     // close BSE switch log file if necessary
                SHOW_WARN(ERROR::FILE_NOT_CLOSED);                                                                          // close failed - show warning
                evolutionStatus = EVOLUTION_STATUS::STOPPED;                                                                // this will cause problems later - stop evolution
            }
        }

        ERRORS->Clean();                                                                                                    // clean the dynamic error catalog

        index++;                                                                                                            // next...
    }
    delete binary;

    if (evolutionStatus == EVOLUTION_STATUS::CONTINUE && index >= nBinaries) evolutionStatus = EVOLUTION_STATUS::DONE;      // set done

    int nBinariesRequested = usingGrid ? (evolutionStatus == EVOLUTION_STATUS::DONE ? nBinariesCreated : -1) : OPTIONS->nBinaries();

    SAY("\nGenerated " << std::to_string(nBinariesCreated) << " of " << (nBinariesRequested < 0 ? "<INCOMPLETE GRID>" : std::to_string(nBinariesRequested)) << " binaries requested");

    if (evolutionStatus == EVOLUTION_STATUS::AIS_EXPLORATORY) {                                                             // AIS said stop?
        SHOW_WARN(ERROR::BINARY_SIMULATION_STOPPED, EVOLUTION_STATUS_LABEL.at(evolutionStatus));                            // yes - show warning
        evolutionStatus = EVOLUTION_STATUS::DONE;                                                                           // set done
    }

    // announce result
    if (!OPTIONS->Quiet()) {
        if (evolutionStatus != EVOLUTION_STATUS::CONTINUE) {                                                                // shouldn't be...
            SAY("\n" << EVOLUTION_STATUS_LABEL.at(evolutionStatus));
        }
        else {
            SHOW_WARN(ERROR::BINARY_SIMULATION_STOPPED, EVOLUTION_STATUS_LABEL.at(EVOLUTION_STATUS::ERROR));                // show warning
        }
    }

    // close BSE logfiles
    // don't check result here - let log system handle it

    (void)LOGGING->CloseAllStandardFiles();


    double cpuSeconds = (clock() - clockStart) / (double) CLOCKS_PER_SEC;                                                   // stop CPU timer and calculate seconds

    auto wallEnd = std::chrono::system_clock::now();                                                                        // stop wall timer
    std::time_t timeEnd = std::chrono::system_clock::to_time_t(wallEnd);                                                    // get end time and date

    SAY("\nEnd generating binaries at " << std::ctime(&timeEnd));
    SAY("Clock time = " << cpuSeconds << " CPU seconds");


    std::chrono::duration<double> wallSeconds = wallEnd - wallStart;                                                        // elapsed seconds

    int wallHH = (int)(wallSeconds.count() / 3600.0);                                                                       // hours
    int wallMM = (int)((wallSeconds.count() - ((double)wallHH * 3600.0)) / 60.0);                                           // minutes
    int wallSS = (int)(wallSeconds.count() - ((double)wallHH * 3600.0) - ((double)wallMM * 60.0));                          // seconds

    SAY("Wall time  = " << wallHH << ":" << wallMM << ":" << wallSS << " (hh:mm:ss)");

    return std::make_tuple(nBinariesRequested, nBinariesCreated);
}


/*
 * COMPAS main program
 *
 * Does some housekeeping:
 *
 * - starts the Options service (program options)
 * - starts the Log service (for logging and debugging)
 * - starts the Rand service (random number generator)
 *
 * Then evolves either a single or binary star (single star only at the moment...)
 *
 */
int main(int argc, char * argv[]) {

    PROGRAM_STATUS programStatus = PROGRAM_STATUS::CONTINUE;                            // status - initially ok
    
    bool ok = OPTIONS->Initialise(argc, argv);                                          // get the program options from the commandline
    
    if (ok) {                                                                           // have commandline options ok?
                                                                                        // yes
        if (OPTIONS->RequestedHelp()) {                                                 // user requested help?
            utils::SplashScreen();                                                      // yes - show splash screen
//            SAY(cmdline_options);                                                       // and help
            programStatus = PROGRAM_STATUS::SUCCESS;                                    // don't evolve anything
        }
        else if (OPTIONS->RequestedVersion()) {                                         // user requested version?
            SAY("COMPAS v" << VERSION_STRING);                                          // yes, show version string
            programStatus = PROGRAM_STATUS::SUCCESS;                                    // don't evolve anything
        }
    }

    if (programStatus == PROGRAM_STATUS::CONTINUE) {

        InitialiseProfiling;                                                            // initialise profiling functionality

        // start the logging service
        LOGGING->Start(OPTIONS->OutputPathString(),                                     // location of logfiles
                       OPTIONS->OutputContainerName(),                                  // directory to be created for logfiles
                       OPTIONS->LogfileNamePrefix(),                                    // prefix for logfile names
                       OPTIONS->LogLevel(),                                             // log level - determines (in part) what is written to log file
                       OPTIONS->LogClasses(),                                           // log classes - determines (in part) what is written to log file
                       OPTIONS->DebugLevel(),                                           // debug level - determines (in part) what debug information is displayed
                       OPTIONS->DebugClasses(),                                         // debug classes - determines (in part) what debug information is displayed
                       OPTIONS->DebugToFile(),                                          // should debug statements also be written to logfile?
                       OPTIONS->ErrorsToFile(),                                         // should error messages also be written to logfile?
                       DELIMITERValue.at(OPTIONS->LogfileDelimiter()));                 // log record field delimiter

        (void)utils::SplashScreen();                                                    // announce ourselves

        if (!LOGGING->Enabled()) programStatus = PROGRAM_STATUS::LOGGING_FAILED;        // logging failed to start
        else {

            RAND->Initialise();                                                         // initialise the random number service

            int objectsRequested = 0;
            int objectsCreated   = 0;

            if(OPTIONS->SingleStar()) {                                                 // Single star?
                std::tie(objectsRequested, objectsCreated) = EvolveSingleStars();       // yes - evolve single stars
            }
            else {                                                                      // no - binary
                std::tie(objectsRequested, objectsCreated) = EvolveBinaryStars();       // evolve binary stars
            }

            RAND->Free();                                                               // release gsl dynamically allocated memory

            LOGGING->Stop(std::make_tuple(objectsRequested, objectsCreated));           // stop the logging service

            programStatus = PROGRAM_STATUS::SUCCESS;                                    // set program status, and...
        }

        ReportProfiling;                                                                // report profiling statistics
    }

    return static_cast<int>(programStatus);                                             // we're done
}

