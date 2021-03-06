//-----------------------------------------------
// Copyright 2009 Wellcome Trust Sanger Institute
// Written by Jared Simpson (js18@sanger.ac.uk)
// Released under the GPL
//-----------------------------------------------
//
// correct - Correct sequencing errors in reads using the FM-index
//
#include <iostream>
#include <fstream>
#include <sstream>
#include <iterator>
#include "Util.h"
#include "FMIndexWalk.h"
#include "SuffixArray.h"
#include "BWT.h"
#include "SGACommon.h"
#include "OverlapCommon.h"
#include "Timer.h"
#include "BWTAlgorithms.h"
#include "ASQG.h"
#include "gzstream.h"
#include "SequenceProcessFramework.h"
#include "MPExtensionProcess.h"
#include "CorrectionThresholds.h"
#include "KmerDistribution.h"
#include "BWTIntervalCache.h"
#include "MPExtension.h"

//
// Getopt
//
#define SUBPROGRAM "MatepairExtension"
static const char *CORRECT_VERSION_MESSAGE =
SUBPROGRAM " Version " PACKAGE_VERSION "\n"
"Written by Chao-Hung Lee.\n"
"\n"
"Copyright 2016 National Chung Cheng University\n";

static const char *CORRECT_USAGE_MESSAGE =
"Usage: " PACKAGE_NAME " " SUBPROGRAM " [OPTION] ... READSFILE\n"
"Merge mate-pair reads into long sequences via FM-index walk\n"
"\n"
"      --help                           display this help and exit\n"
"      -v, --verbose                    display verbose output\n"
"      -p, --prefix=PREFIX              use PREFIX for the names of the index files (default: prefix of the input file)\n"
"      -o, --outfile=FILE               write the corrected reads to FILE (default: READSFILE.ec.fa)\n"
"      -t, --threads=NUM                use NUM threads for the computation (default: 1)\n"
//"      -a, --algorithm=STR              specify the walking algorithm. STR must be hybrid (merge and kmerize) or merge. (default: hybrid)\n"
"\nMerge parameters:\n"
"      -k, --kmer-size=N                The length of the kmer to use. (default: 31)\n"
"      -x, --kmer-threshold=N           Attempt to correct kmers that are seen less than N times. (default: 3)\n"
"      -L, --max-leaves=N               Number of maximum leaves in the search tree (default: 32)\n"
"      -I, --max-insertsize=N           the maximum insert size (i.e. search depth) (deault: 3000)\n"
"      -c  --min-insertsize=N           the minimum insert size of long sequence is to be reserved (default: 375),usually set 0.125 * max insert size\n"
"      -m, --min-overlap=N           the min overlap (default: 81)\n"
"      -M, --max-overlap=N           the max overlap (default: avg read length*0.9)\n"

"\nReport bugs to " PACKAGE_BUGREPORT "\n\n";

static const char* PROGRAM_IDENT =
PACKAGE_NAME "::" SUBPROGRAM;

namespace opt
{
    static unsigned int verbose;
    static int numThreads = 1;
    static std::string prefix;
    static std::string readsFile;
    static std::string outFile;
	static std::string outFile2;//trimmatepair by chaohung 20151030
	static std::string outFile3;//trimmatepair by chaohung 20151103
    static std::string discardFile;
    static int sampleRate = BWT::DEFAULT_SAMPLE_RATE_SMALL;


    static int kmerLength = 31;
    static int kmerThreshold = 3;
    static bool bLearnKmerParams = false;

    static int maxLeaves=32;
	static int maxInsertSize=3000;
	static int minInsertSize=375;
	static int minOverlap=81;
	static int maxOverlap=-1;
}

static const char* shortopts = "p:t:o:a:k:x:L:I:m:M:c:v";

enum { OPT_HELP = 1, OPT_VERSION, OPT_METRICS, OPT_DISCARD, OPT_LEARN };

static const struct option longopts[] = {
    { "verbose",       no_argument,       NULL, 'v' },
    { "threads",       required_argument, NULL, 't' },
    { "outfile",       required_argument, NULL, 'o' },
    { "prefix",        required_argument, NULL, 'p' },
    //{ "algorithm",     required_argument, NULL, 'a' },
    { "kmer-size",     required_argument, NULL, 'k' },
    { "kmer-threshold",required_argument, NULL, 'x' },
    { "max-leaves",    required_argument, NULL, 'L' },
    { "max-insertsize",required_argument, NULL, 'I' },
	{ "min-insertsize",required_argument, NULL, 'c' },
    { "min-overlap"   ,required_argument, NULL, 'm' },
    { "learn",         no_argument,       NULL, OPT_LEARN },
    { "discard",       no_argument,       NULL, OPT_DISCARD },
    { "help",          no_argument,       NULL, OPT_HELP },
    { "version",       no_argument,       NULL, OPT_VERSION },
    { "metrics",       required_argument, NULL, OPT_METRICS },
    { NULL, 0, NULL, 0 }
};

//
// Main
//
int MatepairExtensionMain(int argc, char** argv)
{
    parseMPExtOptions(argc, argv);

    // Set the error correction parameters
    MPExtensionParameters ecParams;
	BWT *pBWT, *pRBWT;
	SampledSuffixArray* pSSA;

    // Load indices
	#pragma omp parallel
	{
		#pragma omp single nowait
		{	//Initialization of large BWT takes some time, pass the disk to next job
			std::cout << std::endl << "Loading BWT: " << opt::prefix + BWT_EXT << "\n";
			pBWT = new BWT(opt::prefix + BWT_EXT, opt::sampleRate);
		}
		#pragma omp single nowait
		{
			std::cout << "Loading RBWT: " << opt::prefix + RBWT_EXT << "\n";
			pRBWT = new BWT(opt::prefix + RBWT_EXT, opt::sampleRate);
		}
		#pragma omp single nowait
		{
			std::cout << "Loading Sampled Suffix Array: " << opt::prefix + SAI_EXT << "\n";
			pSSA = new SampledSuffixArray(opt::prefix + SAI_EXT, SSA_FT_SAI);
		}
	}

    BWTIndexSet indexSet;
    indexSet.pBWT = pBWT;
    indexSet.pRBWT = pRBWT;
    indexSet.pSSA = pSSA;
    ecParams.indices = indexSet;

	// Sample 100000 kmer counts into KmerDistribution from reverse BWT 
	// Don't sample from forward BWT as Illumina reads are bad at the 3' end
	ecParams.kd = BWTAlgorithms::sampleKmerCounts(opt::minOverlap, 100000, pRBWT);
	ecParams.kd.computeKDAttributes();
	// const size_t RepeatKmerFreq = ecParams.kd.getCutoffForProportion(0.95); 
	std::cout << "Median kmer frequency: " <<ecParams.kd.getMedian() << "\t Std: " <<  ecParams.kd.getSdv() 
					<<"\t 95% kmer frequency: " << ecParams.kd.getCutoffForProportion(0.95)
					<< "\t Repeat frequency cutoff: " << ecParams.kd.getRepeatKmerCutoff() << "\n";
	ecParams.FreqThreshold=ecParams.kd.findFirstLocalMinimum();
    // Open outfiles and start a timer
    std::ostream* pWriter = createWriter(opt::outFile);
    std::ostream* pDiscardWriter = (!opt::discardFile.empty() ? createWriter(opt::discardFile) : NULL);
	std::ostream* pWriter2=NULL;//trimmatepair by chaohung 20151030
	std::ostream* pWriter3=NULL;//trimmatepair by chaohung 20151030
    Timer* pTimer = new Timer(PROGRAM_IDENT);

    ecParams.kmerLength = opt::kmerLength;
    ecParams.printOverlaps = opt::verbose > 0;
	ecParams.maxLeaves = opt::maxLeaves;
	ecParams.maxInsertSize = opt::maxInsertSize;
	ecParams.minInsertSize = opt::minInsertSize;
    ecParams.minOverlap = opt::minOverlap;
    ecParams.maxOverlap = opt::maxOverlap;
	
    // Setup post-processor
	//trimmatepair by chaohung 20151030

	pWriter3 = createWriter(opt::outFile3);
	pWriter2 = createWriter(opt::outFile2);
    MPExtensionPostProcess postProcessor(pWriter, pDiscardWriter, pWriter2, pWriter3, ecParams);

    std::cout << "Merge paired end reads into long reads for " << opt::readsFile << " using \n" 
				<< "min overlap=" <<  ecParams.minOverlap << "\t"
				<< "max overlap=" <<  ecParams.maxOverlap << "\t"
				<< "max leaves=" << opt::maxLeaves << "\t"
				<< "max Insert size=" << opt::maxInsertSize << "\t"
				<< "min Insert size=" << opt::minInsertSize << "\t"
				<< "kmer size=" << opt::kmerLength << "\n\n";

    if(opt::numThreads <= 1)
    {
        // Serial mode
        MPExtensionProcess processor(ecParams);

        SequenceProcessFramework::processSequencesSerial<SequenceWorkItemPair,
                                                         MPExtensionResult,
                                                         MPExtensionProcess,
                                                         MPExtensionPostProcess>(opt::readsFile, &processor, &postProcessor);

    }
    else
    {
        // Parallel mode
        std::vector<MPExtensionProcess*> processorVector;
        for(int i = 0; i < opt::numThreads; ++i)
        {
            MPExtensionProcess* pProcessor = new MPExtensionProcess(ecParams);
            processorVector.push_back(pProcessor);
        }

        SequenceProcessFramework::processSequencesParallel<SequenceWorkItemPair,
                                                           MPExtensionResult,
                                                           MPExtensionProcess,
                                                           MPExtensionPostProcess>(opt::readsFile, processorVector, &postProcessor);



        for(int i = 0; i < opt::numThreads; ++i)
        {
            delete processorVector[i];
        }
    }

    delete pBWT;
    if(pRBWT != NULL)
        delete pRBWT;

    if(pSSA != NULL)
        delete pSSA;

    delete pTimer;
	//By Chaohung_2015_11_03
	if(pWriter2 != NULL)//without delete,will cause wc error
        delete pWriter2;
	if(pWriter3 !=NULL)
		delete pWriter3;
    delete pWriter;
    if(pDiscardWriter != NULL)
        delete pDiscardWriter;
	
    return 0;
}


//
// Handle command line arguments
//
void parseMPExtOptions(int argc, char** argv)
{
	optind=1;	//reset getopt
    std::string algo_str;
    bool die = false;
    for (char c; (c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1;)
    {
        std::istringstream arg(optarg != NULL ? optarg : "");
        switch (c)
        {
            case 'p': arg >> opt::prefix; break;
            //case 'o': arg >> opt::outFile; break;
            case 't': arg >> opt::numThreads; break;
            case 'a': arg >> algo_str; break;
            case 'k': arg >> opt::kmerLength; break;
            case 'x': arg >> opt::kmerThreshold; break;
            case '?': die = true; break;
            case 'v': opt::verbose++; break;
			case 'L': arg >> opt::maxLeaves; break;
			case 'I': arg >> opt::maxInsertSize; break;
			case 'c': arg >> opt::minInsertSize;break;
            case 'm': arg >> opt::minOverlap; break;
            case 'M': arg >> opt::maxOverlap; break;
            case OPT_LEARN: opt::bLearnKmerParams = true; break;
            case OPT_HELP:
                std::cout << CORRECT_USAGE_MESSAGE;
                exit(EXIT_SUCCESS);
            case OPT_VERSION:
                std::cout << CORRECT_VERSION_MESSAGE;
                exit(EXIT_SUCCESS);
        }
    }

    if (argc - optind < 1)
    {
        std::cerr << SUBPROGRAM ": missing arguments\n";
        die = true;
    }
    else if (argc - optind > 1)
    {
        std::cerr << SUBPROGRAM ": too many arguments\n";
        die = true;
    }

    if(opt::numThreads <= 0)
    {
        std::cerr << SUBPROGRAM ": invalid number of threads: " << opt::numThreads << "\n";
        die = true;
    }


    if(opt::kmerLength <= 0)
    {
        std::cerr << SUBPROGRAM ": invalid kmer length: " << opt::kmerLength << ", must be greater than zero\n";
        die = true;
    }

    if(opt::kmerThreshold <= 0)
    {
        std::cerr << SUBPROGRAM ": invalid kmer threshold: " << opt::kmerThreshold << ", must be greater than zero\n";
        die = true;
    }
	if(opt::maxInsertSize <=0)
	{
		std::cerr << SUBPROGRAM ": invalid max insert size: " << opt::maxInsertSize << ", must be greater than zero\n";
        die = true;
	}
	if(opt::minInsertSize <=0)
	{
		std::cerr << SUBPROGRAM ": invalid min insert size: " << opt::minInsertSize << ", must be greater than zero\n";
        die = true;
	}

    if (die)
    {
        std::cout << "\n" << CORRECT_USAGE_MESSAGE;
        exit(EXIT_FAILURE);
    }

    // Parse the input filenames
    opt::readsFile = argv[optind++];

    if(opt::prefix.empty())
    {
        opt::prefix = stripFilename(opt::readsFile);
    }

    // Set the correction threshold
    if(opt::kmerThreshold <= 0)
    {
        std::cerr << "Invalid kmer support threshold: " << opt::kmerThreshold << "\n";
        exit(EXIT_FAILURE);
    }
    CorrectionThresholds::Instance().setBaseMinSupport(opt::kmerThreshold);

    std::string out_prefix = stripFilename(opt::readsFile);

	//By Chaohung_2015_01_22
	if(!opt::outFile.empty())
		opt::outFile.clear();
	if(!opt::outFile2.empty())
		opt::outFile2.clear();
	if(!opt::outFile3.empty())
		opt::outFile3.clear();
	if(!opt::discardFile.empty())
		opt::discardFile.clear();
	opt::outFile = out_prefix + ".trimmed.fa";
	opt::outFile2 = out_prefix + ".merge.fa";
	opt::outFile3 = out_prefix + ".shortIS.fa";
	opt::discardFile = out_prefix + ".polluted.fa";
		
}
