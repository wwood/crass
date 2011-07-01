// File: WorkHorse.cpp
// Original Author: Michael Imelfort 2011
// --------------------------------------------------------------------
//
// OVERVIEW:
//
// Implementation of WorkHorse functions
// 
// --------------------------------------------------------------------
//  Copyright  2011 Michael Imelfort and Connor Skennerton
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
// --------------------------------------------------------------------
//
//                        A
//                       A B
//                      A B R
//                     A B R A
//                    A B R A C
//                   A B R A C A
//                  A B R A C A D
//                 A B R A C A D A
//                A B R A C A D A B 
//               A B R A C A D A B R  
//              A B R A C A D A B R A 
//
// system includes
#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <zlib.h>  
#include <fstream>
#include <algorithm>

// local includes
#include "WorkHorse.h"
#include "libcrispr.h"
#include "LoggerSimp.h"
#include "crass_defines.h"
#include "NodeManager.h"
#include "ReadHolder.h"
#include "SeqUtils.h"
#include "SmithWaterman.h"
#include "StlExt.h"

bool sortDirectRepeatByLength( const std::string &a, const std::string &b)
{
    return a.length() > b.length();
}

WorkHorse::~WorkHorse()
{
//    //-----
//    // destructor
//    //

    // clean up all the NodeManagers
    DR_ListIterator dr_iter = mDRs.begin();
    while(dr_iter != mDRs.end())
    {
        if(NULL != dr_iter->second)
        {
            delete dr_iter->second;
            dr_iter->second = NULL;
        }
        dr_iter++;
    }
    mDRs.clear();
    
    // clear the reads!
    clearReadMap( &mReads);
}

void WorkHorse::clearReadList(ReadList * tmp_list)
{
    //-----
    // clear all the reads from the readlist
    //
    ReadListIterator read_iter = tmp_list->begin();
    while(read_iter != tmp_list->end())
    {
        if(*read_iter != NULL)
        {
            delete *read_iter;
            *read_iter = NULL;
        }
        read_iter++;
    }
    tmp_list->clear();
}

void WorkHorse::clearReadMap(ReadMap * tmp_map)
{
    //-----
    // clear all the reads from the readlist
    //
    ReadMapIterator read_iter = tmp_map->begin();
    while(read_iter != tmp_map->end())
    {
        clearReadList(read_iter->second);
        if (read_iter->second != NULL)
        {
            delete read_iter->second;
            read_iter->second = NULL;
        }
        read_iter++;
    }
    tmp_map->clear();
}

// do all the work!
int WorkHorse::doWork(std::vector<std::string> seqFiles)
{
    //-----
    // Taken from the old main function
    //
    if(mOpts->max_mismatches == 0)
    {   
        logInfo("Finding CRISPRs using the boyer-moore search algorithm", 1); 
    }
    else
    {   
        logInfo("Finding CRISPRs using the bitap algorithm", 1); 
    }
    
    std::vector<std::string>::iterator seq_iter = seqFiles.begin();
    while(seq_iter != seqFiles.end())
    {
        logInfo("Parsing file: " << *seq_iter, 1);
        
        // Need to make the string into something more old-skool so that
        // the search functions don't cry!
        char input_fastq[CRASS_DEF_FASTQ_FILENAME_MAX_LENGTH] = { '\0' };
        strncpy(input_fastq, seq_iter->c_str(), CRASS_DEF_FASTQ_FILENAME_MAX_LENGTH);
        
        // return value of the search functions
        float aveReadLength;
        
        // direct repeat sequence and unique ID
        lookupTable patternsLookup;
        
        
        // the sequence of whole spacers and their unique ID
        lookupTable readsFound;
        
        // Use a different search routine, depending on if we allow mismatches or not.
        if(mOpts->max_mismatches == 0)
        {   aveReadLength = bmSearchFastqFile(input_fastq, *mOpts, patternsLookup, readsFound,  &mReads); }
        else
        {   aveReadLength = bitapSearchFastqFile(input_fastq, *mOpts, patternsLookup, readsFound, &mReads); }

        logInfo("Average read length: "<<aveReadLength, 2);
        
        // only nessessary in instances where there are short reads
        if (aveReadLength < CRASS_DEF_READ_LENGTH_CUTOFF)
        {
            logInfo("Beginning multipattern matcher", 1);
            //scanForMultiMatches(input_fastq, *mOpts, patternsLookup, readsFound, &mReads);
        }
        
        // There will be an abundance of forms for each direct repeat.
        // We needs to do somes clustering! Then trim and concatenate the direct repeats
        mungeDRs();
        
        // we don't use these tables any more so why print them
        //printFileLookups(*seq_iter, kmerLookup, patternsLookup, spacerLookup);
        
        logInfo("Finished file: " << *seq_iter, 1);
        seq_iter++;
    }
    
    logInfo("all done!", 1);
    return 0;
}

int WorkHorse::mungeDRs(void)
{
    //-----
    // Cluster potential DRs and work out their true sequences
    // make the node managers while we're at it!
    //
    logInfo("Reducing list of potential DRs", 1);
    
    int next_free_GID = 1;
    std::map<std::string, int> k2GID_map;
    std::map<int, bool> groups;
    DR_Cluster DR2GID_map;
    
    // go through all of the read holder objects
    ReadMapIterator read_map_iter = mReads.begin();
    while (read_map_iter != mReads.end()) 
    {
        clusterDRReads(read_map_iter->first, &next_free_GID, &k2GID_map, &DR2GID_map, &groups);
        ++read_map_iter;
    }
        
    // now that we know what our groups are it's time to find the one true direct repeat
    oneDRToRuleThemAll(&DR2GID_map);
    
    DR_ClusterIterator DR2GID_iter = DR2GID_map.begin();
    while(DR2GID_iter != DR2GID_map.end())
    {
        std::cout << DR2GID_iter->first << " : ";
        std::vector<std::string>::iterator vec_it;
        vec_it = DR2GID_iter->second->begin();
        while (vec_it != DR2GID_iter->second->end()) 
        {
            std::cout<<*vec_it<<" + ";
            vec_it++;
        }
        std::cout<<std::endl;
        DR2GID_iter++;
    }

    
    
    // create a crispr node object and add into a node manager
    
    logInfo("Done!", 1);
}

std::string WorkHorse::threadToSmithWaterman(std::vector<std::string> *array)
{
    //-----
    // take in a cluster of direct repeats and smithwaterman them to discover
    // the hidden powers within -- or just the correct direct repeat
    // if the group is small its totes okay to do all vs all
    // but we don't want to kill the machine so if it's a large group
    // we select some of the longer sequences in the group and perform the
    // comparison on just those sequences

    int group_size = array->size() - 1;
    if (CRASS_DEF_MAX_CLUSTER_SIZE_FOR_SW < group_size)
        group_size = CRASS_DEF_MAX_CLUSTER_SIZE_FOR_SW;
        
    // a hash of the alignments from smith waterman and their frequencies
    std::map<std::string, int> alignmentHash;
    
    for (int i = 0; i < group_size; i++) 
    {
        std::cout << std::endl;
        for (int j = i + 1; j <= group_size; j++) 
        {
            stringPair align_concensus = smithWaterman(array->at(i), array->at(j));
            
            // if the alignment length is less than CRASS_DEF_MIN_SW_ALIGNMENT_RATIO% of the string length
            if (align_concensus.first.length() < array->at(j).length() * CRASS_DEF_MIN_SW_ALIGNMENT_RATIO)
            {
                stringPair rev_comp_align_concensus = smithWaterman(array->at(i), reverseComplement(array->at(j)));
                
                if (rev_comp_align_concensus.first.length() > array->at(j).length() * CRASS_DEF_MIN_SW_ALIGNMENT_RATIO)
                {
                    if(mReads.find(rev_comp_align_concensus.first) != mReads.end())
                    {
                        addOrIncrement(alignmentHash, rev_comp_align_concensus.first);
                    }
                    if(mReads.find(rev_comp_align_concensus.second) != mReads.end())
                    {
                        addOrIncrement(alignmentHash, rev_comp_align_concensus.second);
                    }
                }
            }
            else
            {
                if(mReads.find(align_concensus.first) != mReads.end())
                {
                    addOrIncrement(alignmentHash, align_concensus.first);
                }
                if(mReads.find(align_concensus.second) != mReads.end())
                {
                    addOrIncrement(alignmentHash, align_concensus.second);
                }
            }
        }
    }
    
    // The dr with the highest number of alignments *should* be the one we're looking for
    int max_val = 0;
    std::string the_true_DR = "**unset**";
    std::map<std::string, int>::iterator cluster_iter = alignmentHash.begin();
    while (cluster_iter != alignmentHash.end()) 
    {
        int score = (mReads.find(cluster_iter->first)->second)->size() * cluster_iter->second;
        std::cout << cluster_iter->first << " : " << cluster_iter->second << " (occ) * " << (mReads.find(cluster_iter->first)->second)->size() << " (reads) = " << score << std::endl;
        if (score > max_val) 
        {
            max_val = score;
            the_true_DR = cluster_iter->first;
        }
        cluster_iter++;
    }
    return the_true_DR;
}

// wrapper for smith waterman to fix up the positions of the direct repeats
void inline WorkHorse::clenseClusters()
{
}

void WorkHorse::oneDRToRuleThemAll(DR_Cluster * DR2GID_map)
{
    //-----
    // Each DR_Cluster contains multiple variants on the true DR
    // But which one is real?
    //
    
    DR_ClusterIterator DR2GID_iter = DR2GID_map->begin();
    std::string the_true_DR = "unset";
    
    while (DR2GID_iter != DR2GID_map->end()) 
    {
        std::cout << DR2GID_iter->first << ":" << std::endl;
        
        // sort the vector from the longest to the shortest
        std::sort(DR2GID_iter->second->begin(), DR2GID_iter->second->end(), sortDirectRepeatByLength);
        
        the_true_DR = threadToSmithWaterman(DR2GID_iter->second);

        logInfo("Clustering has revealed the one true direct repeat: "<< the_true_DR, 5);
        std::cout << "Clustering has revealed the one true direct repeat: " << the_true_DR << std::endl;
        // now we use this "true" DR to fix the start stop indexes
        
        ++DR2GID_iter;
    }
}

bool WorkHorse::clusterDRReads(std::string DR, int * nextFreeGID, std::map<std::string, int> * k2GIDMap, DR_Cluster * DR2GIDMap, std::map<int, bool> * groups)
{
    //-----
    // hash a DR!
    //
    int hash_size = 6;      // cutting 6 mers
    int str_len = DR.length();
    int off = str_len - hash_size;
    int num_mers = off + 1;
    
    //***************************************
    //***************************************
    //***************************************
    //***************************************
    // LOOK AT ME!
    // 
    // Here we declare the minimum criteria for membership when clustering
    // this is not cool!
    int min_clust_membership_count = mOpts->kmer_size;
    // 
    //***************************************
    //***************************************
    //***************************************
    //***************************************
    
    // STOLED FROM SaSSY!!!!
    // First we cut kmers from the sequence then we use these to
    // determine overlaps, finally we make edges
    //
    // When we cut kmers from a read it is like this...
    //
    // XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
    // ------------------------------
    // XXXXXXXXXXXXXXXXXXXXXXXXXX
    // XXXXXXXXXXXXXXXXXXXXXXXXXX
    // XXXXXXXXXXXXXXXXXXXXXXXXXX
    // XXXXXXXXXXXXXXXXXXXXXXXXXX
    // XXXXXXXXXXXXXXXXXXXXXXXXXX
    //
    // so we break the job into three parts
    //
    // XXXX|XXXXXXXXXXXXXXXXXXXXXX|XXXX
    // ----|----------------------|----
    // XXXX|XXXXXXXXXXXXXXXXXXXXXX|
    //  XXX|XXXXXXXXXXXXXXXXXXXXXX|X
    //   XX|XXXXXXXXXXXXXXXXXXXXXX|XX
    //    X|XXXXXXXXXXXXXXXXXXXXXX|XXX
    //     |XXXXXXXXXXXXXXXXXXXXXX|XXXX
    //
    // the first and last part may be a little slow but the middle part can fly through...
    //

    // make a 2d array for the kmers!
    char ** kmers = new char*[num_mers];
    for(int i = 0; i < num_mers; i++)
    {
        kmers[i] = new char [hash_size+1];
    }
    
    int * kmer_offsets = new int[num_mers];              // use these offsets when we cut kmers, they are a component of the algorithm
    for(int i = 0; i < num_mers; i++)
    {
        kmer_offsets[i] = i * -1; // Starts at [0, -1, -2, -3, -4, ...]
    }

    int pos_counter = 0;

    // a slow-ish first part
    while(pos_counter < hash_size)
    {
        for(int j = 0; j < num_mers; j++)
        {
            if(pos_counter >= j)
            {
                kmers[j][kmer_offsets[j]] = DR[pos_counter];
            }
            kmer_offsets[j]++;
        }
        pos_counter++;
    }
    
    // this is the fast part of the loop
    while(pos_counter < off)
    {
        for(int j = 0; j < num_mers; j++)
        {
            if(kmer_offsets[j] >= 0 && kmer_offsets[j] < hash_size)
            {
                kmers[j][kmer_offsets[j]] = DR[pos_counter];
            }
            kmer_offsets[j]++;
        }
        pos_counter++;
    }
    
    // an even slower ending
    while(pos_counter < str_len)
    {
        for(int j = 0; j < num_mers; j++)
        {
            if(kmer_offsets[j] < hash_size)
            {
                kmers[j][kmer_offsets[j]] = DR[pos_counter];
            }
            kmer_offsets[j]++;
        }
        pos_counter++;
    }
    
    //
    // Now the fun stuff begins:
    //
    std::vector<std::string> homeless_kmers;
    std::map<int, int> group_count;
    int group = 0;
    for(int i = 0; i < num_mers; ++i)
    {
        // make it a string!
        kmers[i][hash_size+1]='\0';
        std::string tmp_str(kmers[i]);
        tmp_str = laurenize(tmp_str);
        
        // see if we've seen this kmer before
        std::map<std::string, int>::iterator k2g_iter = k2GIDMap->find(tmp_str);
        if(k2g_iter == k2GIDMap->end())
        {
            // first time we seen this one
            homeless_kmers.push_back(tmp_str);
        }
        else
        {
            // only do this if our guy doesn't belong to a group yet
            if(0 == group)
            {
                // this kmer belongs to a group!
                std::map<int, int>::iterator this_group_iter = group_count.find(k2g_iter->second);
                if(this_group_iter == group_count.end())
                {
                    group_count[k2g_iter->second] = 1;
                }
                else
                {
                    group_count[k2g_iter->second]++;
                    if(min_clust_membership_count <= group_count[k2g_iter->second])
                    {
                        // we have found a group for this mofo!
                        group = k2g_iter->second;
                    }
                }
            }
        }
    }
    
    if(0 == group)
    {
        // we need to make a new group for all the homeless kmers
        group = (*nextFreeGID)++;
        
        // we need to make a new entry in the group map
        (*groups)[group] = true;
        (*DR2GIDMap)[group] = new std::vector<std::string>;
    }
    
    // we need to record the group for this mofo!
    (*DR2GIDMap)[group]->push_back(DR);
    
    // we need to assign all homeless kmers to the group!
    std::vector<std::string>::iterator homeless_iter = homeless_kmers.begin();
    while(homeless_iter != homeless_kmers.end())
    {
        (*k2GIDMap)[*homeless_iter] = group;
        homeless_iter++;
    }

    // clean up
    delete [] kmer_offsets;
    for(int i = 0; i < num_mers; i++)
    {
        delete [] kmers[i];
    }
    delete [] kmers;
    
    
}

//**************************************
// file IO
//**************************************

void WorkHorse::printFileLookups(std::string fileName, lookupTable &kmerLookup , lookupTable &patternsLookup, lookupTable &spacerLookup)
{
    //-----
    // Print all the information from a single round
    //
    logInfo("Printing lookup tables from file: " << fileName << "to " << mOutFileDir, 1);
    
    // Make file names
    std::string kmer_lookup_name = mOutFileDir + CRASS_DEF_DEF_KMER_LOOKUP_EXT;
    std::string patterns_lookup_name = mOutFileDir + CRASS_DEF_DEF_PATTERN_LOOKUP_EXT;
    std::string spacer_lookup_name = mOutFileDir + CRASS_DEF_DEF_SPACER_LOOKUP_EXT;
    
    // Write!
    writeLookupToFile(kmer_lookup_name, kmerLookup);  
    writeLookupToFile(patterns_lookup_name, patternsLookup);
    writeLookupToFile(spacer_lookup_name, spacerLookup);
}

void WorkHorse::writeLookupToFile(string &outFileName, lookupTable &outLookup)
{
    std::ofstream outLookupFile;
    outLookupFile.open(outFileName.c_str());
    
    lookupTable::iterator ter = outLookup.begin();
    while (ter != outLookup.end()) 
    {
        outLookupFile<<ter->first<<"\t"<<ter->second<<endl;
        
        ter++;
    }
    outLookupFile.close();
}

