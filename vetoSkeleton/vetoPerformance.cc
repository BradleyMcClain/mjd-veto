//vetoPerformance for auto processing, Input: List of run numbers. Output: text file & terminal.
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "getopt.h"

#include "TFile.h"
#include "TChain.h"
#include "TGraph.h"
#include "TLegend.h"
#include "TCanvas.h"
#include "TAxis.h"
#include "TH1.h"
#include "TF1.h"
#include "TLine.h"

#include "MJVetoEvent.hh"
#include "MGTEvent.hh"
#include "GATDataSet.hh"

using namespace std;

bool CheckForBadErrors(MJVetoEvent veto, int entry, int isGood, bool verbose) 
{
	bool badError = false;

	if (isGood != 1) {
		int error[18] = {0};
		veto.UnpackErrorCode(isGood,error);
		
		// search for particular errors and set a flag.
		for (int q=0; q<18; q++) 
		{
			//ignore errors 4,7,10,11,12
			if (q != 4 && q != 7  && q != 10 && q != 11 && q != 12 && error[q] == 1) badError=true;
		}
		
		if (badError) {
			if (verbose == true) {
				cout << "Skipped Entry: " << entry << endl;
				veto.Print();
				cout << endl;
			}
			return badError;
		}
	}
	else return badError;	// no bad errors found

	return false;
}

// Place threshold 35 qdc above pedestal location.
// Also check if panel is active (any entries over QDC = 300)
int FindQDCThreshold(TH1F *qdcHist, int panel, bool deactivate) 
{
	// check if panel is active (any entries over QDC = 300)
	int lastNonzeroBin = qdcHist->FindLastBinAbove(1,1);
	if (lastNonzeroBin < 300 && lastNonzeroBin > 0 && deactivate) {
		printf("Panel: %i  Found last nonzero bin: %i  No QDC entries over 300! Deactivating panel...\n",panel,lastNonzeroBin);
		return 9999;
	}
	else if (lastNonzeroBin == -1 && deactivate){
		printf("Error!  Last lastNonzeroBin == -1\n");
		return 9999;
	}

	int firstNonzeroBin = qdcHist->FindFirstBinAbove(1,1);
	qdcHist->GetXaxis()->SetRange(firstNonzeroBin-10,firstNonzeroBin+50);
	//qdcHist->GetXaxis()->SetRangeUser(0,500); //alternate method of finding pedestal
	int bin = qdcHist->GetMaximumBin();
	
	if (firstNonzeroBin == -1){
		printf("ERROR: Panel %i -- First Nonzero: %i  Max Bin: %i  Range: %i to %i\n",panel,firstNonzeroBin,bin,firstNonzeroBin-10,firstNonzeroBin+50);
	}

	double xval = qdcHist->GetXaxis()->GetBinCenter(bin);
	return xval+35;
}

int* GetQDCThreshold(string file, int *arr, string name)
{
	string Name = "";

	// Can either use file name or specify a threshold with "name".
	if (name == "") 
	{
		// strip off path and extension
		Name = file;
		Name.erase(Name.find_last_of("."),string::npos);
		Name.erase(0,Name.find_last_of("\\/")+1);
	}
	else {
		Name = name;
	}

	// open the threshold dictionary
	// generated by vetoThreshFinder
	//
	ifstream InputList("vetoSWThresholds.txt");
	if(!InputList.good()) {
    	cout << "Couldn't open vetoSWThresholds.txt " << endl;
    	return NULL;
    }
	int result[32] = {0};
	string buffer;
	char val[200];
	bool foundRange = false;
	while (!InputList.eof() && !foundRange)
	{
		InputList >> buffer;
		if (buffer == Name) {
			cout << "Found SW threshold values for: " << Name << endl;
			foundRange = true;
			for (int i = 0; i < 32; i++) {
				InputList >> val;
				result[i] = atoi(val);
			}
		}
	}
	if (!foundRange) {
		cout << "Didn't find SW threshold values for this range. \n Using defaults..." << endl;
		for (int j=0;j<32;j++) result[j]=500;
	}
	memcpy(arr,result,sizeof(result));

	return arr;
}

double InterpTime(int entry, vector<double> times, vector<double> entries, vector<bool> badScaler)
{
	if ((times.size() != entries.size()) || times.size() != badScaler.size()) 
	{
		cout << "Vectors are different sizes!\n";  
		if (entry >= (int)times.size()) 
			cout << "Entry is larger than number of entries in vector!\n";
		return -1; 
	}
	
	double iTime = 0;

	double lower = 0;
	double upper = 0;
	if (!badScaler[entry]) iTime = times[entry];
	else 
	{
		for (int i = entry; i < (int)entries.size(); i++) 
		{
			if (badScaler[i] == 0) { upper = times[i]; break; }
		}
		for (int j = entry; j > 0; j--)
		{
			if (badScaler[j] == 0) { lower = times[j]; break; }
		}

		iTime = (upper + lower)/2.0;
	}

	return iTime;
}

void vetoPerformance(string Input, int *thresh, bool runBreakdowns) 
{
	// input a list of run numbers
	ifstream InputList(Input.c_str());
	if(!InputList.good()) {
    	cout << "Couldn't open " << Input << endl;
    	return;
    }
    int filesScanned = 0;	// 1-indexed.

    // output a TXT file
	string Name = Input;
	Name.erase(Name.find_last_of("."),string::npos);
	Name.erase(0,Name.find_last_of("\\/")+1);
	Char_t OutputFile[200];
	sprintf(OutputFile,"./vPerf_%s.txt",Name.c_str());
	ofstream errordat;
	errordat.open (OutputFile);
	
	// global counters
	const int nErrs = 27;
	int globalErrorCount[nErrs] = {0};
	int globalRunsWithErrors[nErrs] = {0};
	int RunsWithBadLED = 0;
	int runThresh[32] = {0};	// run-by-run threshold
	int prevThresh[32] = {0};	
	long totEntries = 0;
	long totDuration = 0;
	long totLivetime = 0;
	int totErrorCount = 0;
	int globalRunsWithSeriousErrors = 0;
	int globalSeriousErrorCount = 0;
	char hname[50];

	//define lastprevrun vetoevent holder
	MJVetoEvent lastprevrun;	//DO NOT CLEAR

	// ==========================loop over input files==========================
	//
	while(!InputList.eof())
	{
		int run = 0;
		InputList >> run;
		filesScanned++;
		
		// initialize
		GATDataSet *ds = new GATDataSet(run);
		TChain *v = ds->GetVetoChain();
		long vEntries = v->GetEntries();
		MJTRun *vRun = new MJTRun();
		MGTBasicEvent *vEvent = new MGTBasicEvent(); 
		unsigned int mVeto = 0;
		uint32_t vBits = 0;
		v->SetBranchAddress("run",&vRun);
		v->SetBranchAddress("mVeto",&mVeto);
		v->SetBranchAddress("vetoEvent",&vEvent);
		v->SetBranchAddress("vetoBits",&vBits);
		
		long start = (long)vRun->GetStartTime();
		long stop = (long)vRun->GetStopTime();
		double duration = (double)(stop - start);
		totEntries += vEntries;
		totDuration += (long)duration;
		double livetime = 0;
		
		// run-by-run variables
		int ErrorCountPerRun[nErrs] = {0};
		vector<double> LocalEntryTime;
		vector<double> LocalEntryNum;
		vector<bool> LocalBadScalers;	

		// run-by-run histos and graphs
		sprintf(hname,"%d_LEDDeltaT",run);
		TH1D *LEDDeltaT = new TH1D(hname,hname,100000,0,100); // 0.001 sec/bin

		printf("\n======= Scanning run %i, %li entries. =======\n",run,vEntries);
		MJVetoEvent prev;
		MJVetoEvent first;
		MJVetoEvent last;
		bool foundFirst = false;
		int firstGoodEntry = 0;
		int pureLEDcount = 0;
		bool errorRunBools[nErrs] = {0};
		double xTime = 0;
		double lastGoodTime = 0;
		double SBCOffset = 0;
		
		//define runQDC to check threshold shifts
		TH1F *hRunQDC[32];  
		for (int i = 0; i < 32; i++) {
			sprintf(hname,"hRunQDC%d",i);
			hRunQDC[i] = new TH1F(hname,hname,500,0,500);
		}
		
		
		// ====================== First loop over entries =========================
		for (int i = 0; i < vEntries; i++)
		{
			v->GetEntry(i);
			MJVetoEvent veto;
			veto.SetSWThresh(thresh);	
	    	int isGood = veto.WriteEvent(i,vRun,vEvent,vBits,run,true); // true: force-write event with errors.

	    	// count up error types
	    	if (isGood != 1) 
	    	{	    		
	    		for (int j=0; j<18; j++) if (veto.GetError(j)==1) 
	    		{
					ErrorCountPerRun[j]++;
	    			errorRunBools[j]=true;
	    		}
	    	}
				
	    	// find event time and fill vectors
			if (!veto.GetBadScaler()) {
				LocalBadScalers.push_back(0);
				xTime = veto.GetTimeSec();
			}
			else {
				LocalBadScalers.push_back(1);
				xTime = ((double)i / vEntries) * duration;
			}
			
	    	// fill vectors
	    	// (the time vectors are revised in the second loop)
			LocalEntryNum.push_back(i);		
			LocalEntryTime.push_back(xTime);
			
			// skip bad entries (true = print contents of skipped event)
	    	if (CheckForBadErrors(veto,i,isGood,false)) continue;
			
    		// Save the first good entry number for the SBC offset
			if (!foundFirst && veto.GetTimeSBC() > 0 && veto.GetTimeSec() > 0 && errorRunBools[4] == false) {
				first = veto;
				foundFirst = true;
				firstGoodEntry = i;
			}
			
			//check if first good entry isn't acutally first good entry
			if (foundFirst && errorRunBools[1]){
				foundFirst = false;
			}
			
	    	// very simple LED tag 
			if (veto.GetMultip() > 20) {
				LEDDeltaT->Fill(veto.GetTimeSec()-prev.GetTimeSec());
				pureLEDcount++;
			}
			
			// end of loop : save things
			prev = veto;
			lastGoodTime = xTime;
			veto.Clear();
		} //end of first loop over entries

		// if duration is corrupted, use the last good timestamp as the duration.
		if (duration == 0) {
			printf("Corrupted duration. Using last good timestamp method: %.2f\n",lastGoodTime-first.GetTimeSec());
			duration = lastGoodTime-first.GetTimeSec();
			totDuration += duration;
		}
		
		if (stop == 0){
			livetime = lastGoodTime - first.GetTimeSec();
		}
		else livetime = stop - first.GetTimeSec();
		totLivetime += (long)livetime;

		// find the SBC offset		
		SBCOffset = first.GetTimeSBC() - first.GetTimeSec();
		printf("First good entry: %i  |  SBCOffset: %.2f  |  firstScalerTime: %lf  |  firstSBCTime: %lf  |  firstScalerIndex: %ld\n",firstGoodEntry,SBCOffset,first.GetTimeSec(),first.GetTimeSBC(),first.GetScalerIndex());

		// find the LED frequency
		printf("\"Simple\" LED count: %i.  Approx rate: %.3f\n",pureLEDcount,pureLEDcount/duration);
		double LEDrms = 0;
		double LEDfreq = 0;
		int dtEntries = LEDDeltaT->GetEntries();
		if (dtEntries > 0) {
			int maxbin = LEDDeltaT->GetMaximumBin();
			LEDDeltaT->GetXaxis()->SetRange(maxbin-100,maxbin+100); // looks at +/- 0.1 seconds of max bin.
			LEDrms = LEDDeltaT->GetRMS();
			LEDfreq = 1/LEDDeltaT->GetMean();
		}
		else {
			printf("Warning! No multiplicity > 20 events!!\n");
			LEDrms = 9999;
			LEDfreq = 9999;
		}
		double LEDperiod = 1/LEDfreq;
		printf("Histo method: LED_f: %.8f LED_t: %.8f RMS: %8f\n",LEDfreq,LEDperiod,LEDrms);
		delete LEDDeltaT;

		// set a flag for "bad LED" (usually a short run causes it)
		// and replace the period with the "simple" one if possible
		bool badLEDFreq = false;
		if (LEDperiod > 9 || vEntries < 100) 
		{
			printf("Warning: Short run.\n");
			if (pureLEDcount > 3) {
				printf("   From histo method, LED freq is %.2f.\n   Reverting to the approx rate (%.2fs) ... \n"
					,LEDfreq,(double)pureLEDcount/duration);
				LEDperiod = duration/pureLEDcount;
			}
			else { 
				printf("   Warning: LED info is corrupted!  Will not use LED period information for this run.\n");
				LEDperiod = 9999;
				badLEDFreq = true;
			}
		}
		if (badLEDFreq) RunsWithBadLED++;
		if (LEDperiod > 9 || LEDperiod < 5) {
			ErrorCountPerRun[25]++;
			errorRunBools[25] = true;
		}
	
		// ====================== Second loop over entries =========================
		//
		double STime = 0;
		double STimePrev = 0;
		int SIndex = 0;
		int SIndexPrev = 0;
		int EventNumPrev_good = 0;
		int EventNum = 0;
		double SBCTime = 0;
		double SBCTimePrev = 0;
		double TSdifference = 0;	//a running total of the time difference between the scaler and SBC timestamps
		prev.Clear();
		
		//-------------------------------------------------------------------------------------------------------------------------------------------------------------------------
		for (int i = 0; i < vEntries; i++)
		{
			EventNum = i;
			// this time we don't skip anything until all the time information is found.
			v->GetEntry(i);
			MJVetoEvent veto;
			veto.SetSWThresh(thresh);	
	    	int isGood = veto.WriteEvent(i,vRun,vEvent,vBits,run,true);	// true: force-write event with errors.
			
	    	// find event time 
			if (!veto.GetBadScaler()) {
				xTime = veto.GetTimeSec();
				STime = veto.GetTimeSec();
				SIndex = veto.GetScalerIndex();
				if(run > 8557 && veto.GetTimeSBC() < 2000000000) SBCTime = (veto.GetTimeSBC() - SBCOffset);
				
			}
			else if (run > 8557 && veto.GetTimeSBC() < 2000000000) {
				xTime = veto.GetTimeSBC() - SBCOffset;
				double interpTime = InterpTime(i,LocalEntryTime,LocalEntryNum,LocalBadScalers);
				printf("Entry %i : SBC method: %.2f  Interp method: %.2f  sbc-interp: %.2f\n",i,xTime,interpTime,xTime-interpTime);
			}
			else {
				double eTime = ((double)i / vEntries) * duration;
				xTime = InterpTime(i,LocalEntryTime,LocalEntryNum,LocalBadScalers);
				printf("Entry %i : Entry method: %.2f  Interp method: %.2f  eTime-interp: %.2f\n",i,eTime,xTime,eTime-xTime);
			}
			LocalEntryTime[i] = xTime;	// replace entry with the more accurate one
					
			if (veto.GetError(1)) {
				printf("Error[1] Missing Packet. Run: %d  |  entry: %d  |  Scaler Index: %ld  |  Scaler Time: %f  |  SBC Time: %f\n",run,i,veto.GetScalerIndex(),veto.GetTimeSec(),veto.GetTimeSBC());
			}	
			
			//track Event Count Changes/resets			
			if (veto.GetSEC() == 0 && i != 0 && i > firstGoodEntry) {
				printf("Error[19] SEC Reset. Run: %d  |  entry: %d  |  Index: %ld  |  SEC: %ld  |  prevSEC: %ld\n",run,i,veto.GetScalerIndex(),veto.GetSEC(),prev.GetSEC());
				ErrorCountPerRun[19]++;
				errorRunBools[19] = true;
			}
			
			if (veto.GetQEC() == 0 && i != 0 && i > firstGoodEntry){
				printf("Error[21] QEC1 Reset. Run: %d  |  entry: %d  |  Index: %ld  |  QEC1: %ld  |  prevQEC1: %ld\n",run,i,veto.GetScalerIndex(),veto.GetQEC(),prev.GetQEC());
				ErrorCountPerRun[21]++;
				errorRunBools[21] = true;
			}
			
			if (veto.GetQEC2() == 0 && i != 0 && i > firstGoodEntry){
				printf("Error[23] QEC2 Reset. Run: %d  |  entry: %d  |  Index: %ld  |  QEC2: %ld  |  prevQEC2: %ld\n",run,i,veto.GetScalerIndex(),veto.GetQEC2(),prev.GetQEC2());
				ErrorCountPerRun[23]++;
				errorRunBools[23] = true;
			}

			if(abs(veto.GetSEC() - prev.GetSEC()) > EventNum-EventNumPrev_good && i > firstGoodEntry && !veto.GetSEC() != 0) {
				printf("Error[20] SEC Change. Run: %d  |  entry: %d  |  xTime: %f  |  Index: %ld  |  SEC: %ld  |  prevSEC: %ld\n",run,i,xTime,veto.GetScalerIndex(),veto.GetSEC(),prev.GetSEC()); 
				ErrorCountPerRun[20]++;
				errorRunBools[20] = true;
			}
			
			if(abs(veto.GetQEC() - prev.GetQEC()) > EventNum-EventNumPrev_good && i > firstGoodEntry && veto.GetQEC() != 0) {
				printf("Error[22] QEC1 Change. Run: %d  |  entry: %d  |  xTime: %f  |  Index: %ld  |  QEC1: %ld  |  prevQEC1: %ld\n",run,i,xTime,veto.GetQDC1Index(),veto.GetQEC(),prev.GetQEC()); 
				ErrorCountPerRun[22]++;
				errorRunBools[22] = true;
			}
			
			if(abs(veto.GetQEC2() - prev.GetQEC2()) > EventNum-EventNumPrev_good && i > firstGoodEntry && veto.GetQEC2() != 0) {
				printf("Error[24] QEC2 Change. Run: %d  |  entry: %d  |  xTime: %f  |  Index: %ld  |  QEC2: %ld  |  prevQEC2: %ld\n",run,i,xTime,veto.GetQDC2Index(),veto.GetQEC2(),prev.GetQEC2()); 
				ErrorCountPerRun[24]++;
				errorRunBools[24] = true;
			}

			if (STime > 0 && SBCTime >0 && SBCOffset != 0 && !veto.GetError(1)  && i > firstGoodEntry){
				if( fabs((STime - STimePrev) - (SBCTime - SBCTimePrev)) > 2){	
					printf("Error[18] Scaler/SBC Desynch. Run: %d  |  Entry: %d  |  DeltaT (adjusted): %f  |  DeltaT: %f  |  Prev TSdifference: %f  |  Scaler DeltaT: %f  |  ScalerIndex: %d  |  PrevScalerIndex: %d  |  (rough)LED count: %f\n|  ScalerTime: %f  |  SBCTime: %f\n",run,i,fabs(fabs(STime - SBCTime) - TSdifference),fabs(STime - SBCTime),TSdifference,STime-STimePrev,SIndex,SIndexPrev,(STime-first.GetTimeSec())/LEDperiod,STime,SBCTime); 
					TSdifference = STime - SBCTime;
					ErrorCountPerRun[18]++;
					errorRunBools[18] = true;
				}	
			}
	
			if (i == vEntries-1) {
				printf("Run %d Scaler/SBC duration difference: %f\n",run, STime - SBCTime);
			}
			
			// save previous xTime
			STimePrev = STime;
			SBCTimePrev = SBCTime;
			SIndexPrev = SIndex;
			STime = 0;
			SBCTime = 0;
			SIndex = 0;
			
			// skip bad entries (true = print contents of skipped event)
	    	if (CheckForBadErrors(veto,i,isGood,false)) continue;
	    	
	    	for (int j = 0; j < 32; j++) { 
				hRunQDC[j]->Fill(veto.GetQDC(j));
			}
			
			// end of loop : save things
			prev = veto;
			EventNumPrev_good = EventNum; //save last good event number to search for unexpected SEC/QEC changes
			EventNum = 0;
			if (i == vEntries-1){
				last = veto;
				lastprevrun = last;
			}	
			veto.Clear();
		}

		// Calculate the run-by-run threshold location.
		// Throw a warning if a pedestal shifts by more than 5%.
		for (int c = 0; c < 32; c++) 
		{
			runThresh[c] = FindQDCThreshold(hRunQDC[c],c,false);
			double ratio = (double)runThresh[c]/prevThresh[c];
			if (filesScanned !=1 && (ratio > 1.1 || ratio < 0.9)) 
			{
				printf("Warning! Found pedestal shift! Panel: %i  Previous: %i  This run: %i \n"
					,c,prevThresh[c],runThresh[c]);
				errorRunBools[26] = true;
				ErrorCountPerRun[26]++;
			}
			// save threshold for next scan
			prevThresh[c] = runThresh[c];
		}
		
		// ADD TO # of RUNS WITH ERROR COUNT
			for (int q = 0; q < nErrs; q++) {
				if (errorRunBools[q]) {
					globalRunsWithErrors[q]++;
				}	
			}
		
		// clear memory
		for (int c=0;c<32;c++) delete hRunQDC[c];
		
		// calculate # of errors & # of serious errors this run
		int errorsthisrun = 0;
		int seriouserrorsthisrun = 0;
		for (int i = 1; i < nErrs; i++){
			if (i != 10 && i != 11){
				errorsthisrun += ErrorCountPerRun[i];
			}
			if (i == 1 || i > 17) {
				seriouserrorsthisrun += ErrorCountPerRun[i];
				globalSeriousErrorCount += ErrorCountPerRun[i];
			}
		}
		//if this run has errors add to the global counts.
		//globalRunsWithErrors[0] counts how many runs have > 1 error  (print to feresa)
		if (errorsthisrun != 0) globalRunsWithErrors[0]++;
		if (seriouserrorsthisrun != 0) globalRunsWithSeriousErrors++;
		for (int i = 0; i < nErrs; i++) {
			if (ErrorCountPerRun[i] > 0) {
				globalErrorCount[i] += ErrorCountPerRun[i];
				totErrorCount += ErrorCountPerRun[i];
			}
		}
		
		cout << "=================== End Run " << run << ". =====================\n";
		printf("Duration: %f seconds, Livetime: %f seconds.\n",duration,livetime);

		printf("ErrorsThisRun\n"
			"Error 01: %i  |  Error 02: %i  |  Error 03: %i  |  Error 04: %i  |  Error 05: %i\n" 
			"Error 06: %i  |  Error 07: %i  |  Error 08: %i  |  Error 09: %i  |  Error 10: %i\n" 
			"Error 11: %i  |  Error 12: %i  |  Error 13: %i  |  Error 14: %i  |  Error 15: %i\n" 
			"Error 16: %i  |  Error 17: %i  |  Error 18: %i  |  Error 19: %i  |  Error 20: %i\n" 
			"Error 21: %i  |  Error 22: %i  |  Error 23: %i  |  Error 24: %i  |  Error 25: %i\n"
			"Error 26: %i\n\n", ErrorCountPerRun[1],ErrorCountPerRun[2],ErrorCountPerRun[3],
			ErrorCountPerRun[4],ErrorCountPerRun[5],ErrorCountPerRun[6],ErrorCountPerRun[7],
			ErrorCountPerRun[8],ErrorCountPerRun[9],ErrorCountPerRun[10],ErrorCountPerRun[11],
			ErrorCountPerRun[12],ErrorCountPerRun[13],ErrorCountPerRun[14],ErrorCountPerRun[15],
			ErrorCountPerRun[16],ErrorCountPerRun[17],ErrorCountPerRun[18],ErrorCountPerRun[19],
			ErrorCountPerRun[20],ErrorCountPerRun[21],ErrorCountPerRun[22],ErrorCountPerRun[23],
			ErrorCountPerRun[24],ErrorCountPerRun[25],ErrorCountPerRun[26]);
		
		printf("[FIRST EVENT] Run: %d  |  firstSEC: %ld  |  firstQEC: %ld  |  firstQEC2: %ld  |  firstScalerTime: %f  |  firstSBCTime: %f  |  Scaler Index: %ld  |  vEntries: %ld\n",run,first.GetSEC(),first.GetQEC(),first.GetQEC2(),first.GetTimeSec(),first.GetTimeSBC()-SBCOffset,first.GetScalerIndex(),vEntries);
		printf("[LAST EVENT] Run: %d  |  LastSEC: %ld  |  LastQEC: %ld  |  LastQEC2: %ld  |  LastScalerTime: %f  |  LastSBCTime: %f  |  Scaler Index: %ld  |  vEntries: %ld\n",run,last.GetSEC(),last.GetQEC(),last.GetQEC2(),last.GetTimeSec(),last.GetTimeSBC()-SBCOffset,last.GetScalerIndex(),vEntries);

		// end of run cleanup
		LocalBadScalers.clear();
		LocalEntryNum.clear();
		LocalEntryTime.clear();

	}
	
	cout << "\n\n================= END OF SCAN. =====================\n";
	printf("%i runs, %li total events, total duration: %ld seconds, total Livetime: %ld seconds.\n",filesScanned,totEntries,totDuration,totLivetime);

	// check for errors and print summary if we find them.
	bool foundErrors = false;
	for (int i = 0; i < nErrs; i++) { 
		if (globalErrorCount[i] > 0) {
			foundErrors = true; 
			break;
		}
	}
	
	if (foundErrors) 
	{
		printf("\nError summary:\n");
		
		printf("Number of errors: %d  |  Number of runs with > 1 error: %d\n\n"
		,totErrorCount, globalRunsWithErrors[0]);
		printf("Number of runs with badLED tag: %d\n\n", RunsWithBadLED);
		printf("Number of serious errors: %d  |  Number of runs with serious errors: %d\n\n"
		,globalSeriousErrorCount, globalRunsWithSeriousErrors);
		
		for (int i = 0; i < nErrs; i++) 
		{
			if (globalErrorCount[i] > 0) 
			{
				foundErrors = true;
				if (i !=25){
					printf("%i: %i events\t(%.2f%%)\t"
						,i,globalErrorCount[i],100*(double)globalErrorCount[i]/totEntries);
					printf("%i runs\t(%.2f%%)\n"
						,globalRunsWithErrors[i],100*(double)globalRunsWithErrors[i]/filesScanned);
				}
				else {
					printf("%i: %i runs\t(%.2f%%)\n"
						,i,globalRunsWithErrors[i],100*(double)globalRunsWithErrors[i]/filesScanned);
				}		
			}
		}
		
		printf("\nFor reference, error types are:\n");
		cout << "1. Missing channels (< 32 veto datas in event) " << endl;
		cout << "2. Extra Channels (> 32 veto datas in event) " << endl; 
		cout << "3. Scaler only (no QDC data) " << endl;
		cout << "4. Bad Timestamp: FFFF FFFF FFFF FFFF " << endl;
		cout << "5. QDCIndex - ScalerIndex != 1 or 2 " << endl;
		cout << "6. Duplicate channels (channel shows up multiple times) " << endl;
		cout << "7. HW Count Mismatch (SEC - QEC != 1 or 2) " << endl;
		cout << "8. MJTRun run number doesn't match input file" << endl;
		cout << "9. MJTVetoData cast failed (missing QDC data)" << endl;
		cout << "10. Scaler EventCount doesn't match ROOT entry" << endl;
		cout << "11. Scaler EventCount doesn't match QDC1 EventCount" << endl;
		cout << "12. QDC1 EventCount doesn't match QDC2 EventCount" << endl;
		cout << "13. Indexes of QDC1 and Scaler differ by more than 2" << endl;
		cout << "14. Indexes of QDC2 and Scaler differ by more than 2" << endl;
		cout << "15. Indexes of either QDC1 or QDC2 PRECEDE the scaler index" << endl;
		cout << "16. Indexes of either QDC1 or QDC2 EQUAL the scaler index" << endl;
		cout << "17. Unknown Card is present." << endl;
		cout << "18. Scaler & SBC Timestamp Desynch." << endl;
		cout << "19. Scaler Event Count reset." << endl;
		cout << "20. Scaler Event Count increment by > +1." << endl;
		cout << "21. QDC1 Event Count reset." << endl;
		cout << "22. QDC1 Event Count increment by > +1." << endl;
		cout << "23. QDC2 Event Count reset." << endl;
		cout << "24. QDC2 Event Count increment > +1." << endl;
		cout << "25. LED frequency very low/high." << endl;
		cout << "26. Threshold shift by more than 5%." << endl << endl;
		
		cout << "Serious errors are 1, 18, 19, 20, 21, 22, 23, 24, 25, 26." << endl;
	}
	
	errordat.close();
	cout << "\nEnd of vetoPerformance." << endl;
}