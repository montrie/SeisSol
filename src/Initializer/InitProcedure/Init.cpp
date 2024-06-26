#include "Init.hpp"

#include <sstream>

#include "InitIO.hpp"
#include "InitMesh.hpp"
#include "InitModel.hpp"
#include "InitSideConditions.hpp"
#include "Initializer/Parameters/SeisSolParameters.h"
#include "Monitoring/Unit.hpp"
#include "Numerical_aux/Statistics.h"
#include "Parallel/MPI.h"
#include "ResultWriter/ThreadsPinningWriter.h"
#include "SeisSol.h"

namespace {

static void reportDeviceMemoryStatus() {
#ifdef ACL_DEVICE
  device::DeviceInstance& device = device::DeviceInstance::getInstance();
  const auto rank = seissol::MPI::mpi.rank();
  if (device.api->getCurrentlyOccupiedMem() > device.api->getMaxAvailableMem()) {
    std::stringstream stream;

    stream << "Memory of device (" << rank << ") is overloaded." << std::endl
           << "Totally allocated device memory: "
           << UnitByte.formatPrefix(device.api->getCurrentlyOccupiedMem()) << std::endl
           << "Allocated unified memory: "
           << UnitByte.formatPrefix(device.api->getCurrentlyOccupiedUnifiedMem()) << std::endl
           << "Memory capacity of device: "
           << UnitByte.formatPrefix(device.api->getMaxAvailableMem());

    logError() << stream.str();
  } else {
    double fraction = device.api->getCurrentlyOccupiedMem() /
                      static_cast<double>(device.api->getMaxAvailableMem());
    const auto summary = seissol::statistics::parallelSummary(fraction * 100.0);
    logInfo(rank) << "occupied memory on devices (%):"
                  << " mean =" << summary.mean << " std =" << summary.std << " min =" << summary.min
                  << " median =" << summary.median << " max =" << summary.max;
  }
#endif
}

static void initSeisSol(seissol::SeisSol& seissolInstance) {
  const auto& seissolParams = seissolInstance.getSeisSolParameters();

  // set g
  seissolInstance.getGravitationSetup().acceleration =
      seissolParams.model.gravitationalAcceleration;

  // initialization procedure
  seissol::initializer::initprocedure::initMesh(seissolInstance);
  seissol::initializer::initprocedure::initModel(seissolInstance);
  seissol::initializer::initprocedure::initSideConditions(seissolInstance);
  seissol::initializer::initprocedure::initIO(seissolInstance);

  // set up simulator
  auto& sim = seissolInstance.simulator();
  sim.setUsePlasticity(seissolParams.model.plasticity);
  sim.setFinalTime(seissolParams.timeStepping.endTime);
}

static void reportHardwareRelatedStatus(seissol::SeisSol& seissolInstance) {
  reportDeviceMemoryStatus();

  const auto& seissolParams = seissolInstance.getSeisSolParameters();
  writer::ThreadsPinningWriter pinningWriter(seissolParams.output.prefix);
  pinningWriter.write(seissolInstance.getPinning());
}

static void closeSeisSol(seissol::SeisSol& seissolInstance) {
  logInfo(seissol::MPI::mpi.rank()) << "Closing IO.";
  // cleanup IO
  seissolInstance.waveFieldWriter().close();
  seissolInstance.checkPointManager().close();
  seissolInstance.faultWriter().close();
  seissolInstance.freeSurfaceWriter().close();

  // deallocate memory manager
  seissolInstance.deleteMemoryManager();
}

} // namespace

void seissol::initializer::initprocedure::seissolMain(seissol::SeisSol& seissolInstance) {
  initSeisSol(seissolInstance);
  reportHardwareRelatedStatus(seissolInstance);

  // just put a barrier here to make sure everyone is synched
  logInfo(seissol::MPI::mpi.rank()) << "Finishing initialization...";
  seissol::MPI::mpi.barrier(seissol::MPI::mpi.comm());

  seissol::Stopwatch watch;
  logInfo(seissol::MPI::mpi.rank()) << "Starting simulation.";
  watch.start();
  seissolInstance.simulator().simulate(seissolInstance);
  watch.pause();
  watch.printTime("Time spent in simulation:");

  // make sure everyone is really done
  logInfo(seissol::MPI::mpi.rank()) << "Simulation done.";
  seissol::MPI::mpi.barrier(seissol::MPI::mpi.comm());

  closeSeisSol(seissolInstance);
}
