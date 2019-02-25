/*****************************************************************************
 * Copyright (C) 2014 Visualink
 *
 * Authors: Adrien Maglo <adrien@visualink.io>
 *
 * This file is part of Pastec.
 *
 * Pastec is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Pastec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Pastec.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#include <iostream>
#include <string>
#include <sstream>
#include <unistd.h>
#include <cstdlib>
#include <algorithm>
#include <sys/time.h>
#include <assert.h>

#include <orbindex.h>
#include <messages.h>


ORBIndex::ORBIndex(string indexPath, bool cacheImageWords, unsigned int autoSaveInterval)
    : indexPath(indexPath), autoSaveInterval(autoSaveInterval),
      cacheImageWords(cacheImageWords)
{
    // Init the mutex.
    pthread_rwlock_init(&rwLock, NULL);

    // Initialize the nbOccurences table.
    for (unsigned i = 0; i < NB_VISUAL_WORDS; ++i)
        nbOccurences[i] = 0;

    if (indexPath == "")
        indexPath = DEFAULT_INDEX_PATH;

    readIndex(indexPath);

    hasUnsavedChanges = false;

    if (autoSaveInterval > 0) {
        pthread_t thread;
        pthread_create(&thread, NULL, ORBIndex::autoSaveIndex, this);
    }
}


/**
 * @brief Return the number of occurences of a word in an whole index.
 * @param i_wordId the word id.
 * @return the number of occurences.
 */
unsigned ORBIndex::getWordNbOccurences(unsigned i_wordId)
{
    pthread_rwlock_rdlock(&rwLock);
    assert(i_wordId < NB_VISUAL_WORDS);
    unsigned i_ret = nbOccurences[i_wordId];
    pthread_rwlock_unlock(&rwLock);
    return i_ret;
}


ORBIndex::~ORBIndex()
{
    pthread_rwlock_destroy(&rwLock);
}


void ORBIndex::getImagesWithVisualWords(unordered_map<u_int32_t, list<Hit> > &imagesReqHits,
                                     unordered_map<u_int32_t, vector<Hit> > &indexHitsForReq)
{
    pthread_rwlock_rdlock(&rwLock);

    for (unordered_map<u_int32_t, list<Hit> >::const_iterator it = imagesReqHits.begin();
         it != imagesReqHits.end(); ++it)
    {
        const unsigned i_wordId = it->first;
        indexHitsForReq[i_wordId] = indexHits[i_wordId];
    }

    pthread_rwlock_unlock(&rwLock);
}


/**
 * @brief Return the number of words for an image
 * @param i_imageId the image id.
 * @return the number of words.
 * readLock() and unlock MUST be called before and after calling this function.
 */
unsigned ORBIndex::countTotalNbWord(unsigned i_imageId)
{
    unsigned i_ret = nbWords[i_imageId];
    return i_ret;
}


unsigned ORBIndex::getTotalNbIndexedImages()
{
    pthread_rwlock_rdlock(&rwLock);
    unsigned i_ret = nbWords.size();
    pthread_rwlock_unlock(&rwLock);
    return i_ret;
}


/**
 * @brief Add a list of hits to the index.
 * @param  the list of hits.
 */
u_int32_t ORBIndex::addImage(unsigned i_imageId, list<HitForward> hitList)
{
    pthread_rwlock_wrlock(&rwLock);
    hasUnsavedChanges = true;

    if (nbWords.find(i_imageId) != nbWords.end())
    {
        pthread_rwlock_unlock(&rwLock);
        removeImage(i_imageId);
        pthread_rwlock_wrlock(&rwLock);
    }

    for (list<HitForward>::iterator it = hitList.begin(); it != hitList.end(); ++it)
    {
        HitForward hitFor = *it;
        assert(i_imageId = hitFor.i_imageId);
        Hit hitBack;
        hitBack.i_imageId = hitFor.i_imageId;
        hitBack.i_angle = hitFor.i_angle;
        hitBack.x = hitFor.x;
        hitBack.y = hitFor.y;

        if (cacheImageWords)
        {
            imageWords[hitFor.i_imageId].push_back(hitFor.i_wordId);
        }
        indexHits[hitFor.i_wordId].push_back(hitBack);
        nbWords[hitFor.i_imageId]++;
        nbOccurences[hitFor.i_wordId]++;
        totalNbRecords++;
    }
    pthread_rwlock_unlock(&rwLock);

    if (!hitList.empty())
        cout << "Image " << hitList.begin()->i_imageId << " added: "
             << hitList.size() << " hits." << endl;

    return IMAGE_ADDED;
}


/**
 * @brief Remove all the hits of an image.
 * @param i_imageId the image id.
 * @return true on success else false.
 */
u_int32_t ORBIndex::removeImage(const unsigned i_imageId)
{
    pthread_rwlock_wrlock(&rwLock);
    unordered_map<u_int64_t, unsigned>::iterator imgIt =
        nbWords.find(i_imageId);

    if (imgIt == nbWords.end())
    {
        cout << "Image " << i_imageId << " not found." << endl;
        pthread_rwlock_unlock(&rwLock);
        return IMAGE_NOT_FOUND;
    }

    hasUnsavedChanges = true;
    nbWords.erase(imgIt);

    if (cacheImageWords)
    {
        unordered_map<u_int64_t, vector<unsigned> >::iterator imageWordsIt =
            imageWords.find(i_imageId);

        if (imageWordsIt == imageWords.end())
        {
            cout << "Image " << i_imageId << " not found." << endl;
            pthread_rwlock_unlock(&rwLock);
            return IMAGE_NOT_FOUND;
        }

        imageWords.erase(imageWordsIt);
    }

    for (unsigned i_wordId = 0; i_wordId < NB_VISUAL_WORDS; ++i_wordId)
    {
        vector<Hit> &hits = indexHits[i_wordId];
        vector<Hit>::iterator it = hits.begin();

        while (it != hits.end())
        {
            if (it->i_imageId == i_imageId)
            {
                totalNbRecords--;
                nbOccurences[i_wordId]--;
                hits.erase(it);
                break;
            }
            ++it;
        }
    }
    pthread_rwlock_unlock(&rwLock);

    cout << "Image " << i_imageId << " removed." << endl;

    return IMAGE_REMOVED;
}


/**
 * @brief Get a list of hits associated with an image id.
 * @param  the list of hits.
 */
u_int32_t ORBIndex::getImageWords(unsigned i_imageId, float threshold, unordered_map<u_int32_t, list<Hit> > &hitList)
{
    //pthread_rwlock_wrlock(&rwLock);

    const unsigned i_nbTotalIndexedImages = getTotalNbIndexedImages();
    const unsigned i_maxNbOccurences = i_nbTotalIndexedImages > 10000 ?
                                       threshold * i_nbTotalIndexedImages
                                       : i_nbTotalIndexedImages;

    unordered_map<u_int64_t, unsigned>::iterator imgIt =
        nbWords.find(i_imageId);

    if (imgIt == nbWords.end())
    {
        cout << "Image " << i_imageId << " not found." << endl;
        pthread_rwlock_unlock(&rwLock);
        return IMAGE_NOT_FOUND;
    }

    if (cacheImageWords)
    {
        vector<unsigned> &words = imageWords[i_imageId];
        vector<unsigned>::iterator word_it = words.begin();

        while (word_it != words.end())
        {
            unsigned i_wordId = *word_it;

            if (getWordNbOccurences(i_wordId) <= i_maxNbOccurences)
            {
                vector<Hit> &hits = indexHits[i_wordId];
                vector<Hit>::iterator hit_it = hits.begin();

                while (hit_it != hits.end())
                {
                    if (hit_it->i_imageId == i_imageId)
                    {
                        hitList[i_wordId].push_back(*hit_it);
                        break;
                    }
                    ++hit_it;
                }
            }
            ++word_it;
        }
    }
    else
    {
        for (unsigned i_wordId = 0; i_wordId < NB_VISUAL_WORDS; ++i_wordId)
        {
            vector<Hit> &hits = indexHits[i_wordId];
            vector<Hit>::iterator it = hits.begin();

            while (it != hits.end())
            {
                if (it->i_imageId == i_imageId)
                {
                    if (getWordNbOccurences(i_wordId) <= i_maxNbOccurences)
                    {
                        hitList[i_wordId].push_back(*it);
                    }
                    break;
                }
                ++it;
            }
        }
    }

    //pthread_rwlock_unlock(&rwLock);

    cout << "Image " << i_imageId << " found with " << hitList.size() << " words." << endl;

    return OK;
}


/**
 * @brief Read the index and store it in memory.
 * @return true on success else false
 */
bool ORBIndex::readIndex(string backwardIndexPath)
{
    // Open the file.
    BackwardIndexReaderFileAccess indexAccess;
    if (!indexAccess.open(backwardIndexPath))
    {
        cout << "Could not open the backward index file." << endl
             << "Using an empty index." << endl;
        return false;
    }
    else
    {
        pthread_rwlock_wrlock(&rwLock);

        /* Read the table to know where are located the lines corresponding to each
         * visual word. */
        cout << "Reading the numbers of occurences." << endl;
        u_int64_t *wordOffSet = new u_int64_t[NB_VISUAL_WORDS];
        u_int64_t i_offset = NB_VISUAL_WORDS * sizeof(u_int64_t);
        for (unsigned i = 0; i < NB_VISUAL_WORDS; ++i)
        {
            indexAccess.read((char *)(nbOccurences + i), sizeof(u_int64_t));
            wordOffSet[i] = i_offset;
            i_offset += nbOccurences[i] * BACKWARD_INDEX_ENTRY_SIZE;
        }

        /* Count the number of words per image. */
        cout << "Counting the number of words per image." << endl;
        totalNbRecords = 0;
        while (!indexAccess.endOfIndex())
        {
            u_int32_t i_imageId;
            u_int16_t i_angle, x, y;
            indexAccess.read((char *)&i_imageId, sizeof(u_int32_t));
            indexAccess.read((char *)&i_angle, sizeof(u_int16_t));
            indexAccess.read((char *)&x, sizeof(u_int16_t));
            indexAccess.read((char *)&y, sizeof(u_int16_t));
            nbWords[i_imageId]++;
            totalNbRecords++;
        }

        indexAccess.reset();

        cout << "Loading the index in memory." << endl;

        for (unsigned i_wordId = 0; i_wordId < NB_VISUAL_WORDS; ++i_wordId)
        {
            indexAccess.moveAt(wordOffSet[i_wordId]);
            vector<Hit> &hits = indexHits[i_wordId];

            const unsigned i_nbOccurences = nbOccurences[i_wordId];
            hits.resize(i_nbOccurences);

            for (u_int64_t i = 0; i < i_nbOccurences; ++i)
            {
                u_int32_t i_imageId;
                u_int16_t i_angle, x, y;
                indexAccess.read((char *)&i_imageId, sizeof(u_int32_t));
                indexAccess.read((char *)&i_angle, sizeof(u_int16_t));
                indexAccess.read((char *)&x, sizeof(u_int16_t));
                indexAccess.read((char *)&y, sizeof(u_int16_t));
                hits[i].i_imageId = i_imageId;
                hits[i].i_angle = i_angle;
                hits[i].x = x;
                hits[i].y = y;

                if (cacheImageWords)
                {
                    imageWords[i_imageId].push_back(i_wordId);
                }
            }
        }

        indexAccess.close();
        delete[] wordOffSet;

        pthread_rwlock_unlock(&rwLock);

        return true;
    }
}


/**
 * @brief Write the index in memory to a file.
 * @param backwardIndexPath
 * @return the operation code
 */
u_int32_t ORBIndex::write(string backwardIndexPath)
{
    if (backwardIndexPath == "")
        backwardIndexPath = indexPath;

    ofstream ofs;

    ofs.open(backwardIndexPath.c_str(), ios_base::binary);
    if (!ofs.good())
    {
        cout << "Could not open the backward index file." << endl;
        return INDEX_NOT_WRITTEN;
    }

    pthread_rwlock_rdlock(&rwLock);

    cout << "Writing the number of occurences." << endl;
    for (unsigned i = 0; i < NB_VISUAL_WORDS; ++i)
        ofs.write((char *)(nbOccurences + i), sizeof(u_int64_t));

    cout << "Writing the index hits." << endl;
    for (unsigned i = 0; i < NB_VISUAL_WORDS; ++i)
    {
        const vector<Hit> &wordHits = indexHits[i];

        for (unsigned j = 0; j < wordHits.size(); ++j)
        {
            const Hit &hit = wordHits[j];
            ofs.write((char *)(&hit.i_imageId), sizeof(u_int32_t));
            ofs.write((char *)(&hit.i_angle), sizeof(u_int16_t));
            ofs.write((char *)(&hit.x), sizeof(u_int16_t));
            ofs.write((char *)(&hit.y), sizeof(u_int16_t));
        }
    }

    ofs.close();
    cout << "Writing done." << endl;

    hasUnsavedChanges = false;

    pthread_rwlock_unlock(&rwLock);

    return INDEX_WRITTEN;
}


void* ORBIndex::autoSaveIndex(void *index_ptr)
{
    ORBIndex * index = (ORBIndex*)index_ptr;

    cout << "Watching for changes to auto-save..." << endl;

    while(true)
    {
        if (index->hasUnsavedChanges)
        {
            cout << "Auto-saving changes..." << endl;
            index->write("");
        }
        sleep(index->autoSaveInterval);
    }
}

/**
 * @brief Clear the index.
 * @return true on success else false.
 */
u_int32_t ORBIndex::clear()
{
    pthread_rwlock_wrlock(&rwLock);
    hasUnsavedChanges = true;
    // Reset the nbOccurences table.
    for (unsigned i = 0; i < NB_VISUAL_WORDS; ++i)
    {
        nbOccurences[i] = 0;
        indexHits[i].clear();
    }

    nbWords.clear();
    imageWords.clear();
    totalNbRecords = 0;
    pthread_rwlock_unlock(&rwLock);

    cout << "Index cleared." << endl;

    return INDEX_CLEARED;
}


/**
 * @brief Load the index from a file.
 * @param backwardIndexPath the path to the index file.
 * @return the operation code.
 */
u_int32_t ORBIndex::load(string backwardIndexPath)
{
    clear();
    readIndex(backwardIndexPath);

    return INDEX_LOADED;
}


/**
 * @brief List the images ids in the index.
 * @param imageIds the vector to return the images ids.
 * @return the operation code.
 */
u_int32_t ORBIndex::getImageIds(vector<u_int32_t> &imageIds)
{
    imageIds.reserve(nbWords.size());
    for (unordered_map<u_int64_t, unsigned>::const_iterator it = nbWords.begin();
         it != nbWords.end(); ++it)
        imageIds.push_back(it->first);

    return INDEX_IMAGE_IDS;
}


/**
 * @brief Lock for reading the index.
 */
void ORBIndex::readLock()
{
    pthread_rwlock_rdlock(&rwLock);
}


/**
 * @brief Unlock the index.
 */
void ORBIndex::unlock()
{
    pthread_rwlock_unlock(&rwLock);
}
