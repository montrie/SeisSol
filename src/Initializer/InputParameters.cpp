#include "InputParameters.hpp"

#include "InputAux.hpp"
#include "utils/logger.h"

#include <yaml-cpp/yaml.h>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <vector>

using namespace seissol::initializer::parameters;

// converts a string to lower case. Maybe it will trim a string in the future as well.
void sanitize(std::string& input) {
    // https://stackoverflow.com/a/313990
    std::transform(input.begin(), input.end(), input.begin(), [](char c){return std::tolower(c);});
}

// A small helper class which reads a YAML node dictionary. It keeps track of all items that have been read and reports all values which are not used or not used anymore.
// TODO: maybe make the reader more tree-like (i.e. keep a central set on which nodes have been visited), and output all non-understood values at the end and not between sections
class ParameterReader {
public:
    ParameterReader(const YAML::Node& node, bool empty) : node(node), empty(empty) {}

    template<typename T>
    T readWithDefault(const std::string& field, const T& defaultValue) {
        T value = defaultValue;
        if (hasField(field)) {
            value = readUnsafe<T>(field);
        }
        else {
            logDebug() << "The field " << field << " was not specified, using fallback.";
        }
        return value;
    }

    template<typename T>
    T readWithDefaultEnum(const std::string& field, const T& defaultValue, const std::unordered_set<T>& validValues) {
        int value = readWithDefault(field, static_cast<int>(defaultValue));
        if (validValues.find(static_cast<T>(value)) == validValues.end()) {
            logError() << "The field " << field << " had an invalid enum value: " << value;
        }
        return static_cast<T>(value);
    }

    template<typename T>
    T readWithDefaultStringEnum(const std::string& field, const std::string& defaultValue, const std::unordered_map<std::string, T>& validValues) {
        std::string value = readWithDefault(field, defaultValue); // TODO: sanitize string
        sanitize(value);
        if (validValues.find(value) == validValues.end()) {
            logError() << "The field " << field << " had an invalid enum value: " << value;
        }
        return validValues.at(value);
    }

    template<typename T>
    T readOrFail(const std::string& field, const std::string& failMessage) {
        if (hasField(field)) {
            return readUnsafe<T>(field);
        }
        else {
            logError() << "The field " << field << " was not found, but it is required.";
        }
    }

    void warnDeprecatedSingle(const std::string& field) {
        if (hasField(field)) {
            visited.emplace(field);
            logWarning() << "The field " << field << " is no longer in use. You may safely remove it from your parameters file.";
        }
    }

    void warnDeprecated(const std::vector<std::string>& fields) {
        for (const auto& field : fields) {
            warnDeprecatedSingle(field);
        }
    }

    void warnLeftover() {
        for (const auto& subnodes : node) {
            auto field = subnodes.first.as<std::string>();
            if (visited.find(field) == visited.end()) {
                logWarning() << "The field " << field << " is not known to SeisSol.";
            }
        }
    }

    void markUnused(const std::string& field) {
        logDebug() << "The field " << field << " is ignored (regardless of if it exists or not)";
        visited.emplace(field);
    }

    ParameterReader subreader(const std::string& subnodeName) {
        visited.emplace(subnodeName);
        logDebug() << "Entering section " << subnodeName;
        if (hasField(subnodeName)) {
            return ParameterReader(node[subnodeName], false);
        }
        else {
            logDebug() << "Section " << subnodeName << " not found in the given parameter file. Using an empty reader.";
            return ParameterReader(node[subnodeName], true);
        }
        
    }

    bool hasField(const std::string& field) {
        return !empty && node[field];
    }

private:
    template<typename T>
    T readUnsafe(const std::string& field) {
        visited.emplace(field);
        logDebug() << "The field " << field << " was read.";
        try {
            // booleans are stored as integers
            if constexpr(std::is_same<T, bool>::value) {
                return node[field].as<int>() > 0;
            } else {
                return node[field].as<T>();
            }
        } catch(std::exception& e) {
            logError() << "Error while reading field " << field << ": " << e.what();
        }
    }

    bool empty;
    YAML::Node node; // apparently the YAML nodes use a reference semantic. Hence, we do it like this.
    std::unordered_set<std::string> visited;
};

void readModel(ParameterReader& baseReader, SeisSolParameters& ssp) {
    auto reader = baseReader.subreader("equations");

    ssp.model.materialFileName = reader.readOrFail<std::string>("materialfilename", "No material file given.");
    ssp.model.boundaryFileName = reader.readWithDefault("boundaryfilename", std::string(""));

    ssp.model.gravitationalAcceleration = reader.readWithDefault("gravitationalacceleration", 9.81);

    ssp.model.plasticity = reader.readWithDefault("plasticity", false);
    ssp.model.tv = reader.readWithDefault("tv", 0.1);
    ssp.model.useCellHomogenizedMaterial = reader.readWithDefault("usecellhomogenizedmaterial", false);

#if NUMBER_OF_RELAXATION_MECHANISMS > 0
    ssp.model.freqCentral = reader.readOrFail<double>("freqcentral", "equations.freqcentral is needed for the attenuation fitting.");
    ssp.model.freqRatio = reader.readOrFail<double>("freqratio", "equations.freqratio is needed for the attenuation fitting.");
#else
    reader.markUnused("freqcentral");
    reader.markUnused("freqratio");
#endif

    reader.warnDeprecated({"adjoint", "adjfilename", "anisotropy"});
    reader.warnLeftover();
}

void readMesh(ParameterReader& baseReader, SeisSolParameters& ssp) {
    auto reader = baseReader.subreader("meshnml");

    ssp.mesh.meshFileName = reader.readOrFail<std::string>("meshfile", "No mesh file given.");
    ssp.mesh.meshFormat = reader.readWithDefaultStringEnum<MeshFormat>("meshgenerator", "puml", {
        {"gambit3d-fast", MeshFormat::Gambit3D},
        {"netcdf", MeshFormat::Netcdf},
        {"puml", MeshFormat::PUML}
    });

    ssp.mesh.displacement = reader.readWithDefault("displacement", std::array<double, 3>{0,0,0});
    auto scalingX = reader.readWithDefault("scalingmatrixx", std::array<double, 3>{1,0,0});
    auto scalingY = reader.readWithDefault("scalingmatrixy", std::array<double, 3>{0,1,0});
    auto scalingZ = reader.readWithDefault("scalingmatrixz", std::array<double, 3>{0,0,1});
    ssp.mesh.scaling = {scalingX, scalingY, scalingZ};

    ssp.timestepping.vertexWeight.weightElement = reader.readWithDefault("vertexWeightElement", 0);
    ssp.timestepping.vertexWeight.weightDynamicRupture = reader.readWithDefault("vertexWeightDynamicRupture", 0);
    ssp.timestepping.vertexWeight.weightFreeSurfaceWithGravity = reader.readWithDefault("vertexWeightFreeSurfaceWithGravity", 0);

    reader.warnDeprecated({"periodic", "periodic_direction"});
    reader.warnLeftover();
}

void readTimestepping(ParameterReader& baseReader, SeisSolParameters& ssp)
{
    auto reader = baseReader.subreader("discretization");

    ssp.timestepping.cfl = reader.readWithDefault("cfl", 0.5);
    ssp.timestepping.maxTimestep = reader.readWithDefault("fixtimestep", 5000.0);
    ssp.timestepping.lts.rate = reader.readWithDefault("clusteredlts", 2u);
    ssp.timestepping.lts.weighttype = reader.readWithDefault("ltsweighttypeid", 1);

    reader.warnDeprecated({"ckmethod", "dgfineout1d", "fluxmethod", "iterationcriterion", "npoly", "npolyrec", "limitersecurityfactor", "order", "material", "npolymap"});
    reader.warnLeftover();
}

void readInitialization(ParameterReader& baseReader, SeisSolParameters& ssp) {
    auto reader = baseReader.subreader("inicondition");

    ssp.initialization.type = reader.readWithDefaultStringEnum<InitializationType>("cictype", "zero", {
        {"zero", InitializationType::Zero},
        {"planarwave", InitializationType::Planarwave},
        {"superimposedplanarwave", InitializationType::SuperimposedPlanarwave},
        {"travelling", InitializationType::Travelling},
        {"scholte", InitializationType::Scholte},
        {"snell", InitializationType::Snell},
        {"ocean_0", InitializationType::Ocean0},
        {"ocean_1", InitializationType::Ocean1},
        {"ocean_2", InitializationType::Ocean2},
    });
    ssp.initialization.origin = reader.readWithDefault("origin", std::array<double, 3>{0});
    ssp.initialization.kVec = reader.readWithDefault("kvec", std::array<double, 3>{0});
    ssp.initialization.ampField = reader.readWithDefault("ampfield", std::array<double, NUMBER_OF_QUANTITIES>{0});

    reader.warnLeftover();
}

void readOutput(ParameterReader& baseReader, SeisSolParameters& ssp) {
    auto reader = baseReader.subreader("output");

    // general params
    ssp.output.format = reader.readWithDefaultEnum<OutputFormat>("format", OutputFormat::None, {
        OutputFormat::None,
        OutputFormat::Xdmf
    });
    ssp.output.prefix = reader.readOrFail<std::string>("outputfile", "Output file prefix not defined.");
    ssp.output.refinement = reader.readWithDefaultEnum<OutputRefinement>("refinement", OutputRefinement::NoRefine, {
        OutputRefinement::NoRefine,
        OutputRefinement::Refine4,
        OutputRefinement::Refine8,
        OutputRefinement::Refine32
    });
    ssp.output.xdmfWriterBackend = reader.readWithDefaultStringEnum<xdmfwriter::BackendType>("xdmfwriterbackend", "posix", {
        {"posix", xdmfwriter::BackendType::POSIX},
#ifdef USE_HDF
        {"hdf5", xdmfwriter::BackendType::H5},
#endif
    });

    // output time interval
    if (ssp.output.format != OutputFormat::None) {
        ssp.output.interval = reader.readOrFail<double>("timeinterval", "No output interval specified.");
    }
    else {
        reader.markUnused("timeinterval");
    }

    ssp.output.pickDt = reader.readWithDefault("pickdt", 0.0);

    // checkpointing
    ssp.output.checkpointParameters.backend = reader.readWithDefaultStringEnum<seissol::checkpoint::Backend>("checkpointbackend", "none", {
        {"none", seissol::checkpoint::Backend::DISABLED},
        {"posix", seissol::checkpoint::Backend::POSIX},
        {"hdf5", seissol::checkpoint::Backend::HDF5},
        {"mpio", seissol::checkpoint::Backend::MPIO},
        {"mpio_async", seissol::checkpoint::Backend::MPIO_ASYNC},
        {"sionlib", seissol::checkpoint::Backend::SIONLIB}
    });
    ssp.output.checkpointParameters.interval = reader.readWithDefault("checkpointinterval", 0.0);
    ssp.output.checkpointParameters.enabled = ssp.output.checkpointParameters.interval > 0;
    if (ssp.output.checkpointParameters.enabled) {
        ssp.output.checkpointParameters.fileName = reader.readOrFail<std::string>("checkpointfile", "No checkpoint filename given.");
    }
    else {
        reader.markUnused("checkpointfile");
    }

    // output: surface
    ssp.output.outputSurfaceParameters.enabled = reader.readWithDefault("surfaceoutput", false);
    ssp.output.outputSurfaceParameters.interval = reader.readWithDefault("surfaceoutputinterval", 0.0);
    ssp.output.outputSurfaceParameters.enabled &= ssp.output.outputSurfaceParameters.interval > 0;
    ssp.output.outputSurfaceParameters.refinement = reader.readWithDefaultEnum<OutputRefinement>("surfaceoutputrefinement", OutputRefinement::NoRefine, {
        OutputRefinement::NoRefine,
        OutputRefinement::Refine4,
        OutputRefinement::Refine8,
        OutputRefinement::Refine32
    });

    // output: energy
    ssp.output.outputEnergyParameters.enabled = reader.readWithDefault("energyoutput", false);
    ssp.output.outputEnergyParameters.interval = reader.readWithDefault("energyoutputinterval", 0.0);
    ssp.output.outputEnergyParameters.enabled &= ssp.output.outputEnergyParameters.interval > 0;
    ssp.output.outputEnergyParameters.terminalOutput = reader.readWithDefault("energyterminaloutput", false);
    ssp.output.outputEnergyParameters.computeVolumeEnergiesEveryOutput = reader.readWithDefault("computevolumeenergieseveryoutput", true);

    // output: refinement
    ssp.output.outputReceiverParameters.interval = reader.readWithDefault("receiveroutputinterval", 0.0);
    ssp.output.outputReceiverParameters.enabled = ssp.output.outputReceiverParameters.interval > 0;
    ssp.output.outputReceiverParameters.computeRotation = reader.readWithDefault("receivercomputerotation", false);
    ssp.output.outputReceiverParameters.fileName = reader.readOrFail<std::string>("rfilename", "No receiver output file name specified.");

    // output: fault
    ssp.output.faultOutput = reader.readWithDefault("faultoutputflag", false);

    // output mask
    auto iOutputMask = reader.readOrFail<std::string>("ioutputmask", "No output mask given.");
    seissol::initializers::convertStringToMask(iOutputMask, ssp.output.outputMask);

    auto iPlasticityMask = reader.readWithDefault("iplasticitymask", std::string("0 0 0 0 0 0 0"));
    seissol::initializers::convertStringToMask(iPlasticityMask, ssp.output.plasticityMask);

    auto integrationMask = reader.readWithDefault("integrationmask", std::string("0 0 0 0 0 0 0 0 0"));
    seissol::initializers::convertStringToMask(integrationMask, ssp.output.integrationMask);

    // TODO: check if ioutputmaskmaterial is still in use...
    reader.warnDeprecated({"rotation", "interval", "nrecordpoints", "printintervalcriterion", "pickdttype", "ioutputmaskmaterial"});
    reader.warnLeftover();
}

void readEnd(ParameterReader& baseReader, SeisSolParameters& ssp) {
    auto reader = baseReader.subreader("abortcriteria");

    ssp.end.endTime = reader.readWithDefault("endtime", 15.0);
    ssp.end.maxIterations = reader.readWithDefault("maxiterations", 1000000000);

    reader.warnDeprecated({"MaxTolerance", "MaxTolCriterion", "WallTime_h", "Delay_h"});
    reader.warnLeftover();
}

void readSource(ParameterReader& baseReader, SeisSolParameters& ssp) {
    auto reader = baseReader.subreader("sourcetype");

    ssp.source.type = reader.readWithDefaultEnum("type", seissol::sourceterm::SourceType::None, {
        seissol::sourceterm::SourceType::None,
        seissol::sourceterm::SourceType::FsrmSource,
        seissol::sourceterm::SourceType::NrfSource
    });
    if (ssp.source.type != seissol::sourceterm::SourceType::None) {
        ssp.source.fileName = reader.readOrFail<std::string>("filename", "No source file specified.");
    }
    else {
        reader.markUnused("filename");
    }

    reader.warnDeprecated({"Rtype", "nDirac", "nPulseSource", "nRicker"});
    reader.warnLeftover();
}

void SeisSolParameters::readPar(const YAML::Node& baseNode) {
    logInfo() << "Reading SeisSol parameter file...";

    ParameterReader baseReader(baseNode, false);

    readModel(baseReader, *this);
    readMesh(baseReader, *this);
    readTimestepping(baseReader, *this);
    readInitialization(baseReader, *this);
    readOutput(baseReader, *this);
    readSource(baseReader, *this);
    readEnd(baseReader, *this);

    baseReader.warnDeprecated({"rffile", "inflowbound", "inflowboundpwfile", "inflowbounduin", "source110", "source15", "source1618", "source17", "source19", "spongelayer", "sponges"});
    baseReader.warnLeftover();

    logInfo() << "SeisSol parameter file read successfully.";
}

void SeisSolParameters::printInfo() {
    //logInfo() << "SeisSol parameters.";

    // TODO: printing some model info at least would be nice (if still needed)
}
