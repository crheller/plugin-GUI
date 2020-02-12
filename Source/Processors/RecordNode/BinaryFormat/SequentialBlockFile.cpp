/*
------------------------------------------------------------------

This file is part of the Open Ephys GUI
Copyright (C) 2013 Open Ephys

------------------------------------------------------------------

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "SequentialBlockFile.h"

SequentialBlockFile::SequentialBlockFile(int nChannels, int samplesPerBlock) :
m_file(nullptr),
m_nChannels(nChannels),
m_samplesPerBlock(samplesPerBlock),
m_blockSize(nChannels*samplesPerBlock),
m_lastBlockFill(0)
{
	m_memBlocks.ensureStorageAllocated(blockArrayInitSize);
	for (int i = 0; i < nChannels; i++)
		m_currentBlock.add(-1);
}

SequentialBlockFile::~SequentialBlockFile()
{
	//Ensure that all remaining blocks are flushed in order. Keep the last one
	int n = m_memBlocks.size();
	for (int i = 0; i < n - 1; i++)
	{
		m_memBlocks.remove(0);
	}

	LOGD(__FUNCTION__);


	//manually flush the last one to avoid trailing zeroes
	m_memBlocks[0]->partialFlush(m_lastBlockFill * m_nChannels);
}

bool SequentialBlockFile::openFile(String filename)
{
	File file(filename);
	Result res = file.create();
	std::cout << "***Creating file: " << filename << std::endl;
	if (res.failed())
	{
		std::cerr << "Error creating file " << filename << ":" << res.getErrorMessage() << std::endl;
		file.deleteFile();
		Result res = file.create();
		std::cout << "Re-creating file: " << filename << std::endl;
	}
	else {
		std::cout << "Succesfully created file: " << filename << std::endl;
	}
	m_file = file.createOutputStream(streamBufferSize);
	if (!m_file)
	{
		printf("[RN]SequentialBlockFile::openFile returned false\n");
		return false;
	}

	//printf("[RN]SequentialBlockFile::added new FileBlock\n");
	m_memBlocks.add(new FileBlock(m_file, m_blockSize, 0));
	return true;
}

bool SequentialBlockFile::writeChannel(uint64 startPos, int channel, int16* data, int nSamples)
{
	//printf("[RN]Enter SequentialBlockFile::writeChannel\n");
	if (!m_file)
	{
		printf("[RN]SequentialBlockFile::writeChannel returned false: (!m_file)\n");
		return false;
	}

	int bIndex = m_memBlocks.size() - 1;
	if ((bIndex < 0) || (m_memBlocks[bIndex]->getOffset() + m_samplesPerBlock) < (startPos + nSamples))
		allocateBlocks(startPos, nSamples);

	for (bIndex = m_memBlocks.size() - 1; bIndex >= 0; bIndex--)
	{
		if (m_memBlocks[bIndex]->getOffset() <= startPos)
			break;
	}
	if (bIndex < 0)
	{
		printf("\r[RN]SequentialBlockFile: Memory block unloaded ahead of time for chan %d start %d ns %d first %d", channel, startPos, nSamples, m_memBlocks[0]->getOffset()); fflush(stdout);
		for (int i = 0; i < m_nChannels; i++)
			printf("\r CH: %d last block %d", i, m_currentBlock[i]); fflush(stdout);
			//std::cout << "channel " << i << " last block " << m_currentBlock[i] << std::endl;
		return false;
	}
	int writtenSamples = 0;
	int startIdx = startPos - m_memBlocks[bIndex]->getOffset();
	int startMemPos = startIdx*m_nChannels;
	int dataIdx = 0;
	int lastBlockIdx = m_memBlocks.size() - 1;
	//printf("SequentialBlockFile::Entering write loop...\n");
	while (writtenSamples < nSamples)
	{
		int16* blockPtr = m_memBlocks[bIndex]->getData();
		int samplesToWrite = jmin((nSamples - writtenSamples), (m_samplesPerBlock - startIdx));
		for (int i = 0; i < samplesToWrite; i++)
		{
			//if (writtenSamples == 0 && *(data + dataIdx) == 0)
			//{
			//  std::cout << "Found a zero." << std::endl;
			//  break;
			//}

			*(blockPtr + startMemPos + channel + i*m_nChannels) = *(data + dataIdx);
			dataIdx++;
		}
		writtenSamples += samplesToWrite;

		//printf("nSamples: %d, writtenSamples: %d, samplesToWrite: %d\n", nSamples, writtenSamples, samplesToWrite);

		//Update the last block fill index
		size_t samplePos = startIdx + samplesToWrite;
		if (bIndex == lastBlockIdx && samplePos > m_lastBlockFill)
		{
			m_lastBlockFill = samplePos;
		}

		startIdx = 0;
		startMemPos = 0;
		bIndex++;
	}
	m_currentBlock.set(channel, bIndex - 1); //store the last block a channel was written in
	return true;
}

void SequentialBlockFile::allocateBlocks(uint64 startIndex, int numSamples)
{
	//First deallocate full blocks
	//Search for the earliest unused block;
	unsigned int minBlock = 0xFFFFFFFF; //large number;
	for (int i = 0; i < m_nChannels; i++)
	{
		if (m_currentBlock[i] < minBlock)
			minBlock = m_currentBlock[i];
	}

	//Update block indexes
	for (int i = 0; i < m_nChannels; i++)
	{
		m_currentBlock.set(i, m_currentBlock[i] - minBlock);
	}

	m_memBlocks.removeRange(0, minBlock);

	//for (int i = 0; i < minBlock; i++)
	//{
	//Not the most efficient way, as it has to move back all the elements, but it's a simple array of pointers, so it's quick enough
	//  m_memBlocks.remove(0);
	//}

	//Look for the last available position and calculate needed space
	uint64 lastOffset = m_memBlocks.getLast()->getOffset();
	uint64 maxAddr = lastOffset + m_samplesPerBlock - 1;
	uint64 newSpaceNeeded = numSamples - (maxAddr - startIndex);
	int newBlocks = (newSpaceNeeded + m_samplesPerBlock - 1) / m_samplesPerBlock; //Fast ceiling division

	for (int i = 0; i < newBlocks; i++)
	{
		lastOffset += m_samplesPerBlock;
		m_memBlocks.add(new FileBlock(m_file, m_blockSize, lastOffset));
	}
	if (newBlocks > 0)
		m_lastBlockFill = 0; //we've added some new blocks, so the last one will be empty
}

