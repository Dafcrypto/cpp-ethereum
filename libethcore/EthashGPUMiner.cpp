/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file EthashGPUMiner.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 *
 * Determines the PoW algorithm.
 */

#if ETH_ETHASHCL || !ETH_TRUE

#include "EthashGPUMiner.h"
#include <thread>
#include <chrono>
#include <libethash-cl/ethash_cl_miner.h>
using namespace std;
using namespace dev;
using namespace eth;

namespace dev
{
namespace eth
{

class EthashCLHook: public ethash_cl_miner::search_hook
{
public:
	EthashCLHook(EthashGPUMiner* _owner): m_owner(_owner) {}
	EthashCLHook(EthashCLHook const&) = delete;

	void abort()
	{
		{
			UniqueGuard l(x_all);
			if (m_aborted)
				return;
//		cdebug << "Attempting to abort";

			m_abort = true;
		}
		// m_abort is true so now searched()/found() will return true to abort the search.
		// we hang around on this thread waiting for them to point out that they have aborted since
		// otherwise we may end up deleting this object prior to searched()/found() being called.
		m_aborted.wait(true);
//		for (unsigned timeout = 0; timeout < 100 && !m_aborted; ++timeout)
//			std::this_thread::sleep_for(chrono::milliseconds(30));
//		if (!m_aborted)
//			cwarn << "Couldn't abort. Abandoning OpenCL process.";
	}

	void reset()
	{
		UniqueGuard l(x_all);
		m_aborted = m_abort = false;
	}

protected:
	virtual bool found(uint64_t const* _nonces, uint32_t _count) override
	{
//		dev::operator <<(std::cerr << "Found nonces: ", vector<uint64_t>(_nonces, _nonces + _count)) << std::endl;
		for (uint32_t i = 0; i < _count; ++i)
			if (m_owner->report(_nonces[i]))
				return (m_aborted = true);
		return m_owner->shouldStop();
	}

	virtual bool searched(uint64_t _startNonce, uint32_t _count) override
	{
		UniqueGuard l(x_all);
//		std::cerr << "Searched " << _count << " from " << _startNonce << std::endl;
		m_owner->accumulateHashes(_count);
		m_last = _startNonce + _count;
		if (m_abort || m_owner->shouldStop())
			return (m_aborted = true);
		return false;
	}

private:
	Mutex x_all;
	uint64_t m_last;
	bool m_abort = false;
	Notified<bool> m_aborted = {true};
	EthashGPUMiner* m_owner = nullptr;
};

}
}

unsigned EthashGPUMiner::s_platformId = 0;
unsigned EthashGPUMiner::s_deviceId = 0;
unsigned EthashGPUMiner::s_numInstances = 0;
int EthashGPUMiner::s_devices[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

EthashGPUMiner::EthashGPUMiner(ConstructionInfo const& _ci):
	GenericMiner<EthashProofOfWork>(_ci),
	Worker("openclminer" + toString(index())),
	m_hook(new EthashCLHook(this))
{
}

EthashGPUMiner::~EthashGPUMiner()
{
	pause();
	delete m_miner;
	delete m_hook;
}

bool EthashGPUMiner::report(uint64_t _nonce)
{
	Nonce n = (Nonce)(u64)_nonce;
	EthashProofOfWork::Result r = EthashAux::eval(work().seedHash, work().headerHash, n);
	if (r.value < work().boundary)
		return submitProof(Solution{n, r.mixHash});
	return false;
}

void EthashGPUMiner::kickOff()
{
	m_hook->reset();
	startWorking();
}

void EthashGPUMiner::workLoop()
{
	// take local copy of work since it may end up being overwritten by kickOff/pause.
	try {
		WorkPackage w = work();
		cnote << "set work; seed: " << "#" + w.seedHash.hex().substr(0, 8) + ", target: " << "#" + w.boundary.hex().substr(0, 12);
		if (!m_miner || m_minerSeed != w.seedHash)
		{
			unsigned device = s_devices[index()] > -1 ? s_devices[index()] : index();

			if (s_dagLoadMode == DAG_LOAD_MODE_SEQUENTIAL)
			{
				while (s_dagLoadIndex < index()) {
					this_thread::sleep_for(chrono::seconds(1));
				}
			}
			else if (s_dagLoadMode == DAG_LOAD_MODE_SINGLE)
			{
				if (device != s_dagCreateDevice)
				{
					// wait until DAG is created on selected device
					while (s_dagInHostMemory == NULL) {
						this_thread::sleep_for(chrono::seconds(1));
					}
				}
				else
				{
					// reset load index
					s_dagLoadIndex = 0;
				}
			}
			else if (s_dagLoadMode == DAG_LOAD_MODE_SINGLE_KEEP)
			{
				if (device != s_dagCreateDevice)
				{
					// wait until DAG with spec seed is created
					while (s_dagSeed == nullptr || *s_dagSeed != w.seedHash)
						this_thread::sleep_for(chrono::seconds(1));
				}
			}

			cnote << "Initialising miner...";
			m_minerSeed = w.seedHash;

			delete m_miner;
			m_miner = new ethash_cl_miner;

			EthashAux::LightType light;
			light = EthashAux::light(w.seedHash);
			bytesConstRef lightData = light->data();

			if (s_dagLoadMode == DAG_LOAD_MODE_SINGLE_KEEP)
			{
				if (device == s_dagCreateDevice)
				{
					WriteGuard* l = new WriteGuard(s_dagMutex);
					if (s_dagInHostMemory && *s_dagSeed != w.seedHash)
					{
						cout << "Freeing DAG from host" << endl;
						delete[] s_dagInHostMemory;
						//CUDA_SAFE_CALL(cudaFreeHost((void*)s_dagInHostMemory));
						s_dagInHostMemory = nullptr;
					}

					cout << "Generating DAG seed " << w.seedHash << endl;

					// need to generate DAG and copy to host
					m_miner->init(light->light,
						lightData.data(),
						lightData.size(),
						s_platformId,
						device,
						true,
						&s_dagInHostMemory);

					// DAG created and loaded on host, set dagSeed
					h256* tmp = s_dagSeed;
					s_dagSeed = new h256(w.seedHash);
					if (tmp != nullptr)
						delete tmp;

					delete l;
				}
				else
				{
					// only need to copy DAG from the host
					ReadGuard* l = new ReadGuard(s_dagMutex);
					m_miner->init(light->light,
						lightData.data(),
						lightData.size(),
						s_platformId,
						device,
						true,
						&s_dagInHostMemory);
					delete l;
				}
			}
			else
			{
				m_miner->init(light->light,
					lightData.data(),
					lightData.size(),
					s_platformId,
					device,
					s_dagLoadMode == DAG_LOAD_MODE_SINGLE,
					&s_dagInHostMemory);
			}
			s_dagLoadIndex++;

			if (s_dagLoadMode == DAG_LOAD_MODE_SINGLE)
			{
				if (s_dagLoadIndex >= s_numInstances && s_dagInHostMemory)
				{
					// all devices have loaded DAG, we can free now
					delete[] s_dagInHostMemory;
					s_dagInHostMemory = nullptr;

					cout << "Freeing DAG from host" << endl;
				}
			}
		}

		uint64_t upper64OfBoundary = (uint64_t)(u64)((u256)w.boundary >> 192);
		uint64_t startN;
		if (w.exSizeBits >= 0)
			startN = w.startNonce | ((uint64_t)index() << (64 - 4 - w.exSizeBits)); // this can support up to 16 devices
		m_miner->search(w.headerHash.data(), upper64OfBoundary, *m_hook, (w.exSizeBits >= 0), startN);
	}
	catch (cl::Error const& _e)
	{
		delete m_miner;
		m_miner = nullptr;
		cwarn << "Error GPU mining: " << _e.what() << "(" << _e.err() << ")";
	}
}

void EthashGPUMiner::pause()
{
	m_hook->abort();
	stopWorking();
}

std::string EthashGPUMiner::platformInfo()
{
	return ethash_cl_miner::platform_info(s_platformId, s_deviceId);
}

unsigned EthashGPUMiner::getNumDevices()
{
	return ethash_cl_miner::getNumDevices(s_platformId);
}

void EthashGPUMiner::listDevices()
{
	return ethash_cl_miner::listDevices();
}

bool EthashGPUMiner::configureGPU(
	unsigned * _localWorkSize,
	unsigned * _globalWorkSizeMultiplier,
	unsigned _platformId,
	unsigned _deviceId,
	bool _allowCPU,
	unsigned _extraGPUMemory,
	uint64_t _currentBlock,
	unsigned _dagLoadMode,
	unsigned _dagCreateDevice
)
{
	s_dagLoadMode = _dagLoadMode;
	s_dagCreateDevice = _dagCreateDevice;

	s_platformId = _platformId;
	s_deviceId = _deviceId;

	for (int i = 0; i != 16; ++i)
	{
		_localWorkSize[i] = ((_localWorkSize[i] + 7) / 8) * 8;
		_globalWorkSizeMultiplier[i] *= _localWorkSize[i];
	}

	if (!ethash_cl_miner::configureGPU(
			_platformId,
			_localWorkSize,
			_globalWorkSizeMultiplier,
			_allowCPU,
			_extraGPUMemory,
			_currentBlock
			)
	)
	{
		cout << "No GPU device with sufficient memory was found. Can't GPU mine. Remove the -G argument" << endl;
		return false;
	}
	return true;
}

#endif
