#include "Factory.h"

#include "FrictionLaws/FrictionLaws.h"
#include "FrictionLaws/ThermalPressurization/NoTP.h"
#include "FrictionLaws/ThermalPressurization/ThermalPressurization.h"

#ifdef ACL_DEVICE_OFFLOAD
namespace friction_law_impl = seissol::dr::friction_law::gpu;
#else
namespace friction_law_impl = seissol::dr::friction_law;
#endif

namespace seissol::dr::factory {
std::unique_ptr<AbstractFactory> getFactory(dr::DRParameters& drParameters) {
  switch (drParameters.frictionLawType) {
  case FrictionLawType::NoFault:
    return std::make_unique<NoFaultFactory>(drParameters);
  case FrictionLawType::ImposedSlipRatesYoffe:
    return std::make_unique<ImposedSlipRatesYoffeFactory>(drParameters);
  case FrictionLawType::ImposedSlipRatesGaussian:
    return std::make_unique<ImposedSlipRatesGaussianFactory>(drParameters);
  case FrictionLawType::LinearSlipWeakening:
    return std::make_unique<LinearSlipWeakeningFactory>(drParameters);
  // Prakash-Clifton regularisation for bimaterial faults: see (Pelties et al. 2014)
  case FrictionLawType::LinearSlipWeakeningBimaterial:
    return std::make_unique<LinearSlipWeakeningBimaterialFactory>(drParameters);
  case FrictionLawType::RateAndStateAgingLaw:
    return std::make_unique<RateAndStateAgingFactory>(drParameters);
  case FrictionLawType::RateAndStateSlipLaw:
    return std::make_unique<RateAndStateSlipFactory>(drParameters);
  case FrictionLawType::RateAndStateVelocityWeakening:
    logError() << "friction law 7 currently disabled";
    return std::unique_ptr<AbstractFactory>(nullptr);
  case FrictionLawType::RateAndStateAgingNucleation:
    logError() << "friction law 101 currently disabled";
    return std::unique_ptr<AbstractFactory>(nullptr);
  case FrictionLawType::RateAndStateFastVelocityWeakening:
    return std::make_unique<RateAndStateFastVelocityWeakeningFactory>(drParameters);
  default:
    logError() << "unknown friction law";
    return nullptr;
  }
}

Products NoFaultFactory::produce() {
  return {std::make_unique<seissol::initializers::DynamicRupture>(),
          std::make_unique<initializers::NoFaultInitializer>(drParameters),
          std::make_unique<friction_law::NoFault>(drParameters),
          std::make_unique<output::OutputManager>(new output::NoFault)};
}

Products LinearSlipWeakeningFactory::produce() {
  return {std::make_unique<seissol::initializers::LTS_LinearSlipWeakening>(),
          std::make_unique<initializers::LinearSlipWeakeningInitializer>(drParameters),
          std::make_unique<
              friction_law_impl::LinearSlipWeakeningLaw<friction_law_impl::NoSpecialization>>(
              drParameters),
          std::make_unique<output::OutputManager>(new output::LinearSlipWeakening)};
}

Products RateAndStateAgingFactory::produce() {
  if (drParameters.isThermalPressureOn) {
    return {
        std::make_unique<seissol::initializers::LTS_RateAndState>(),
        std::make_unique<initializers::RateAndStateInitializer>(drParameters),
        std::make_unique<friction_law::AgingLaw<friction_law::ThermalPressurization>>(drParameters),
        std::make_unique<output::OutputManager>(new output::RateAndStateThermalPressurization)};
  } else {
    return {std::make_unique<seissol::initializers::LTS_RateAndState>(),
            std::make_unique<initializers::RateAndStateInitializer>(drParameters),
            std::make_unique<friction_law::AgingLaw<friction_law::NoTP>>(drParameters),
            std::make_unique<output::OutputManager>(new output::RateAndState)};
  }
}

Products RateAndStateSlipFactory::produce() {
  if (drParameters.isThermalPressureOn) {
    return {
        std::make_unique<seissol::initializers::LTS_RateAndState>(),
        std::make_unique<initializers::RateAndStateInitializer>(drParameters),
        std::make_unique<friction_law::SlipLaw<friction_law::ThermalPressurization>>(drParameters),
        std::make_unique<output::OutputManager>(new output::RateAndStateThermalPressurization)};
  } else {
    return {std::make_unique<seissol::initializers::LTS_RateAndState>(),
            std::make_unique<initializers::RateAndStateInitializer>(drParameters),
            std::make_unique<friction_law::SlipLaw<friction_law::NoTP>>(drParameters),
            std::make_unique<output::OutputManager>(new output::RateAndState)};
  }
}

Products LinearSlipWeakeningBimaterialFactory::produce() {
  return {std::make_unique<seissol::initializers::LTS_LinearSlipWeakeningBimaterial>(),
          std::make_unique<initializers::LinearSlipWeakeningBimaterialInitializer>(drParameters),
          std::make_unique<friction_law::LinearSlipWeakeningLaw<friction_law::BiMaterialFault>>(
              drParameters),
          std::make_unique<output::OutputManager>(new output::LinearSlipWeakeningBimaterial)};
}

Products ImposedSlipRatesYoffeFactory::produce() {
  return {std::make_unique<seissol::initializers::LTS_ImposedSlipRatesYoffe>(),
          std::make_unique<initializers::ImposedSlipRatesYoffeInitializer>(drParameters),
          std::make_unique<friction_law::ImposedSlipRates<friction_law::YoffeSTF>>(drParameters),
          std::make_unique<output::OutputManager>(new output::ImposedSlipRates)};
}

Products ImposedSlipRatesGaussianFactory::produce() {
  return {std::make_unique<seissol::initializers::LTS_ImposedSlipRatesGaussian>(),
          std::make_unique<initializers::ImposedSlipRatesGaussianInitializer>(drParameters),
          std::make_unique<friction_law::ImposedSlipRates<friction_law::GaussianSTF>>(drParameters),
          std::make_unique<output::OutputManager>(new output::ImposedSlipRates)};
}

Products RateAndStateFastVelocityWeakeningFactory::produce() {
  if (drParameters.isThermalPressureOn) {
    return {
        std::make_unique<seissol::initializers::LTS_RateAndStateThermalPressurization>(),
        std::make_unique<initializers::RateAndStateThermalPressurizationInitializer>(drParameters),
        std::make_unique<
            friction_law::FastVelocityWeakeningLaw<friction_law::ThermalPressurization>>(
            drParameters),
        std::make_unique<output::OutputManager>(new output::RateAndStateThermalPressurization)};
  } else {
    return {
        std::make_unique<seissol::initializers::LTS_RateAndStateFastVelocityWeakening>(),
        std::make_unique<initializers::RateAndStateFastVelocityInitializer>(drParameters),
        std::make_unique<friction_law::FastVelocityWeakeningLaw<friction_law::NoTP>>(drParameters),
        std::make_unique<output::OutputManager>(new output::RateAndState)};
  }
}
} // namespace seissol::dr::factory