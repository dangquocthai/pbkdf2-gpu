/*
 * Copyright (C) 2015, Ondrej Mosnacek <omosnacek@gmail.com>
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation: either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "processingunit.h"

#include "libpbkdf2-gpu-common/alignment.h"

#include <cstring>

#define DEBUG_BUFFER_SIZE 4

namespace libpbkdf2 {
namespace compute {
namespace opencl {

ProcessingUnit::ProcessingUnit(
        const DeviceContext *context, std::size_t batchSize,
        Logger *logger)
    : context(context), batchSize(batchSize), logger(logger)
{
    auto computeContext = context->getParentContext();
    auto hfContext = computeContext->getParentContext();

    auto hashAlg = hfContext->getHashAlgorithm();
    std::size_t ibl = hashAlg->getInputBlockLength();
    std::size_t obl = hashAlg->getOutputBlockLength();

    std::size_t inputLength = ibl;
    std::size_t outputLength = ALIGN(obl, computeContext->getDerivedKeyLength());

    inputSize = inputLength / sizeof(cl_uint);
    outputSize = outputLength / sizeof(cl_uint);

    inputDataBuffer = std::unique_ptr<cl_uint[]>(new cl_uint[inputSize]);
    outputDataBuffer = std::unique_ptr<cl_uint[]>(new cl_uint[outputSize]);

    outputBlocks = outputLength / obl;

    auto &clContext = hfContext->getContext();
    auto queueProps = context->getDevice().getInfo<CL_DEVICE_QUEUE_PROPERTIES>();
    profilingEnabled = (queueProps & CL_QUEUE_PROFILING_ENABLE) != 0;
    cmdQueue = cl::CommandQueue(clContext, context->getDevice(),
                                profilingEnabled ? CL_QUEUE_PROFILING_ENABLE : 0);

    inputBufferSize = inputLength * batchSize;
    outputBufferSize = outputLength * batchSize;

    inputBuffer = cl::Buffer(clContext, CL_MEM_READ_ONLY, inputBufferSize);
    outputBuffer = cl::Buffer(clContext, CL_MEM_WRITE_ONLY, outputBufferSize);
    debugBuffer = cl::Buffer(clContext, CL_MEM_WRITE_ONLY, DEBUG_BUFFER_SIZE);

    mappedInputBuffer = cmdQueue.enqueueMapBuffer(inputBuffer, true, CL_MAP_WRITE, 0, inputBufferSize);
    mappedOutputBuffer = nullptr;

    kernel = cl::Kernel(computeContext->getProgram(), "pbkdf2_kernel");
    kernel.setArg<cl::Buffer>(0, inputBuffer);
    kernel.setArg<cl::Buffer>(1, outputBuffer);
    kernel.setArg<cl::Buffer>(2, computeContext->getSaltBuffer());
    kernel.setArg<cl_uint>   (3, outputBlocks);
    kernel.setArg<cl_uint>   (4, computeContext->getIterationCount());
    kernel.setArg<cl_uint>   (5, batchSize);
    kernel.setArg<cl::Buffer>(6, debugBuffer);
}

ProcessingUnit::PasswordWriter::PasswordWriter(
        ProcessingUnit &parent, std::size_t index)
{
    dest = (cl_uint *)parent.mappedInputBuffer + index;

    count = parent.batchSize;
    inputSize = parent.inputSize;

    auto computeContext = parent.context->getParentContext();
    auto hfContext = computeContext->getParentContext();
    hashAlg = hfContext->getHashAlgorithm();

    buffer = std::unique_ptr<cl_uint[]>(new cl_uint[inputSize * count]);
}

void ProcessingUnit::PasswordWriter::moveForward(std::size_t offset)
{
    dest += offset;
}

void ProcessingUnit::PasswordWriter::moveBackwards(std::size_t offset)
{
    dest -= offset;
}

void ProcessingUnit::PasswordWriter::setPassword(
        const void *pw, std::size_t pwSize) const
{
    std::size_t ibl = hashAlg->getInputBlockLength();
    std::size_t obl = hashAlg->getOutputBlockLength();

    cl_uint *buffer = this->buffer.get();
    if (pwSize > ibl) {
        /* pre-hash password according to HMAC spec: */
        hashAlg->computeDigest(pw, pwSize, buffer);
        std::memset((char *)buffer + obl, 0, ibl - obl);
    } else {
        std::memcpy((char *)buffer, pw, pwSize);
        std::memset((char *)buffer + pwSize, 0, ibl - pwSize);
    }

    cl_uint *dest2 = dest;
    for (std::size_t row = 0; row < inputSize; row++) {
        *dest2 = buffer[row];
        dest2 += count;
    }
}

ProcessingUnit::DerivedKeyReader::DerivedKeyReader(
        ProcessingUnit &parent, std::size_t index)
{
    count = parent.batchSize;
    outputSize = parent.outputSize;
    outputBlockCount = parent.outputBlocks;
    outputBlockSize = outputSize / outputBlockCount;

    buffer = std::unique_ptr<cl_uint[]>(new cl_uint[outputSize * count]);
    src = (cl_uint *)parent.mappedOutputBuffer + index * outputBlockCount;
}

void ProcessingUnit::DerivedKeyReader::moveForward(std::size_t offset)
{
    src += offset * outputBlockCount;
}

void ProcessingUnit::DerivedKeyReader::moveBackwards(std::size_t offset)
{
    src -= offset * outputBlockCount;
}

const void *ProcessingUnit::DerivedKeyReader::getDerivedKey() const
{
    auto dst = buffer.get();
    for (std::size_t outputBlock = 0; outputBlock < outputBlockCount; outputBlock++) {
        auto src = this->src + outputBlock;
        for (std::size_t row = 0; row < outputBlockSize; row++) {
            *dst = *src;
            src += count * outputBlockCount;
            dst += 1;
        }
    }
    return buffer.get();
}

void ProcessingUnit::beginProcessing()
{
    *logger << "Starting processing..." << std::endl;

    if (mappedInputBuffer != nullptr) {
        cmdQueue.enqueueUnmapMemObject(inputBuffer, mappedInputBuffer);
    }
    if (mappedOutputBuffer != nullptr) {
        cmdQueue.enqueueUnmapMemObject(outputBuffer, mappedOutputBuffer);
    }

    cmdQueue.enqueueNDRangeKernel(kernel, cl::NullRange, cl::NDRange(batchSize, outputBlocks), cl::NullRange);

    mappedInputBuffer = cmdQueue.enqueueMapBuffer(inputBuffer, false, CL_MAP_WRITE, 0, inputBufferSize);
    mappedOutputBuffer = cmdQueue.enqueueMapBuffer(outputBuffer, false, CL_MAP_READ, 0, outputBufferSize, nullptr, &event);
}

void ProcessingUnit::endProcessing()
{
    *logger << "Waiting for processing to end..." << std::endl;
    event.wait();
    *logger << "Processing ended." << std::endl;
    if (profilingEnabled) {
        //*logger << event.getProfilingInfo<CL_PROFILING_COMMAND_QUEUED>() << std::endl;
    }
    event = cl::Event();
}

} // namespace opencl
} // namespace compute
} // namespace libpbkdf2
