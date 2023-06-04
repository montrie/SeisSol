#!/usr/bin/env python3
##
# @file
# This file is part of SeisSol.
#
# @author Carsten Uphoff (c.uphoff AT tum.de, http://www5.in.tum.de/wiki/index.php/Carsten_Uphoff,_M.Sc.)
#
# @section LICENSE
# Copyright (c) 2019, SeisSol Group
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its
#    contributors may be used to endorse or promote products derived from this
#    software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#
# @section DESCRIPTION
#

import numpy as np
from abc import ABC, abstractmethod
from common import generate_kernel_name_prefix
from common import tensor_to_numpy
from common import numpy_to_tensor

from multSim import OptionalDimTensor
from yateto import Tensor, Scalar, simpleParameterSpace
from yateto.ast.node import Add
from yateto.ast.transformer import DeduceIndices, EquivalentSparsityPattern
from yateto.input import parseXMLMatrixFile, parseJSONMatrixFile
from yateto.util import tensor_from_constant_expression, tensor_collection_from_constant_expression
from yateto.memory import CSCMemoryLayout
from yateto.util import create_collection


class ADERDGBase(ABC):
  def __init__(self, order, multipleSimulations, matricesDir):
    self.order = order

    self.alignStride = lambda name: True
    if multipleSimulations > 1:
      self.alignStride = lambda name: name.startswith('fP')
    transpose = multipleSimulations > 1
    self.transpose = lambda name: transpose
    self.t = (lambda x: x[::-1]) if transpose else (lambda x: x)

    self.db = parseXMLMatrixFile('{}/matrices_{}.xml'.format(matricesDir, self.numberOf3DBasisFunctions()), transpose=self.transpose, alignStride=self.alignStride)
    clonesQP = {
      'v': [ 'evalAtQP' ],
      'vInv': [ 'projectQP' ]
    }
    self.db.update( parseXMLMatrixFile('{}/plasticity_ip_matrices_{}.xml'.format(matricesDir, order), clonesQP, transpose=self.transpose, alignStride=self.alignStride))
    self.db.update(parseJSONMatrixFile('{}/sampling_directions.json'.format(matricesDir)))
    self.db.update(parseJSONMatrixFile('{}/mass_{}.json'.format(matricesDir, order)))

    qShape = (self.numberOf3DBasisFunctions(), self.numberOfQuantities())
    self.Q = OptionalDimTensor('Q', 's', multipleSimulations, 0, qShape, alignStride=True)
    self.I = OptionalDimTensor('I', 's', multipleSimulations, 0, qShape, alignStride=True)

    Aplusminus_spp = self.flux_solver_spp()
    self.AplusT = Tensor('AplusT', Aplusminus_spp.shape, spp=Aplusminus_spp)
    self.AminusT = Tensor('AminusT', Aplusminus_spp.shape, spp=Aplusminus_spp)
    Tshape = (self.numberOfExtendedQuantities(), self.numberOfExtendedQuantities())
    trans_spp = self.transformation_spp()
    self.T = Tensor('T', trans_spp.shape, spp=trans_spp)
    trans_inv_spp = self.transformation_inv_spp()
    self.Tinv = Tensor('Tinv', trans_inv_spp.shape, spp=trans_inv_spp)
    godunov_spp = self.godunov_spp()
    self.QgodLocal = Tensor('QgodLocal', godunov_spp.shape, spp=godunov_spp)
    self.QgodNeighbor = Tensor('QgodNeighbor', godunov_spp.shape, spp=godunov_spp)

    self.oneSimToMultSim = Tensor('oneSimToMultSim', (self.Q.optSize(),), spp={(i,): '1.0' for i in range(self.Q.optSize())})

    self.db.update(
      parseJSONMatrixFile('{}/nodal/nodalBoundary_matrices_{}.json'.format(matricesDir,
                                                                           self.order),
                          {},
                          alignStride=self.alignStride,
                          transpose=self.transpose,
                          namespace='nodal')
    )
    self.db.update(
      parseXMLMatrixFile(f"{matricesDir}/nodal/gravitational_energy_matrices_{self.order}.xml",
                         alignStride=self.alignStride)
    )


    # Note: MV2nTo2m is Vandermonde matrix from nodal to modal representation WITHOUT mass matrix factor
    self.V2nTo2JacobiQuad = tensor_from_constant_expression('V2nTo2JacobiQuad', self.db.V2mTo2JacobiQuad['ik'] * \
                                                             self.db.MV2nTo2m['kj'],
                                                             target_indices='ij')

    self.INodal = OptionalDimTensor('INodal',
                                    's',
                                    False, #multipleSimulations,
                                    0,
                                    (self.numberOf2DBasisFunctions(), self.numberOfQuantities()),
                                    alignStride=True)

    project2nFaceTo3m = tensor_collection_from_constant_expression(
      base_name='project2nFaceTo3m',
      expressions=lambda i: self.db.rDivM[i]['jk'] * self.db.V2nTo2m['kl'],
      group_indices=range(4),
      target_indices='jl')

    self.db.update(project2nFaceTo3m)

    selectVelocitySpp = np.zeros((self.numberOfQuantities(), 3))
    selectVelocitySpp[6:9,0:3] = np.eye(3)
    self.selectVelocity = Tensor('selectVelocity', selectVelocitySpp.shape, selectVelocitySpp, CSCMemoryLayout)

    self.selectTractionSpp = np.zeros((self.numberOfQuantities(), 3), dtype=bool)
    self.selectTractionSpp[0,0] = True
    self.selectTractionSpp[3,1] = True
    self.selectTractionSpp[5,2] = True

    self.tractionPlusMatrix = Tensor('tractionPlusMatrix', self.selectTractionSpp.shape, self.selectTractionSpp, CSCMemoryLayout)
    self.tractionMinusMatrix = Tensor('tractionMinusMatrix', self.selectTractionSpp.shape, self.selectTractionSpp, CSCMemoryLayout)

  def numberOf2DBasisFunctions(self):
    return self.order*(self.order+1)//2

  def numberOf3DBasisFunctions(self):
    return self.order*(self.order+1)*(self.order+2)//6

  def numberOf3DQuadraturePoints(self):
    return (self.order+1)**3

  def godunov_spp(self):
    shape = (self.numberOfQuantities(), self.numberOfQuantities())
    return np.ones(shape, dtype=bool)

  def flux_solver_spp(self):
    shape = (self.numberOfQuantities(), self.numberOfExtendedQuantities())
    return np.ones(shape, dtype=bool)

  def transformation_spp(self):
    shape = (self.numberOfExtendedQuantities(), self.numberOfExtendedQuantities())
    return np.ones(shape, dtype=bool)

  def transformation_inv_spp(self):
    return self.godunov_spp()

  @abstractmethod
  def numberOfQuantities(self):
    pass

  @abstractmethod
  def numberOfExtendedQuantities(self):
    pass

  @abstractmethod
  def extendedQTensor(self):
    pass

  @abstractmethod
  def starMatrix(self, dim):
    pass

  def addInit(self, generator):
    fluxScale = Scalar('fluxScale')
    computeFluxSolverLocal = self.AplusT['ij'] <= fluxScale * self.Tinv['ki'] * self.QgodLocal['kq'] * self.starMatrix(0)['ql'] * self.T['jl']
    generator.add('computeFluxSolverLocal', computeFluxSolverLocal)

    computeFluxSolverNeighbor = self.AminusT['ij'] <= fluxScale * self.Tinv['ki'] * self.QgodNeighbor['kq'] * self.starMatrix(0)['ql'] * self.T['jl']
    generator.add('computeFluxSolverNeighbor', computeFluxSolverNeighbor)

    QFortran = Tensor('QFortran', (self.numberOf3DBasisFunctions(), self.numberOfQuantities()))
    multSimToFirstSim = Tensor('multSimToFirstSim', (self.Q.optSize(),), spp={(0,): '1.0'})
    if self.Q.hasOptDim():
      copyQToQFortran = QFortran['kp'] <= self.Q['kp'] * multSimToFirstSim['s']
    else:
      copyQToQFortran = QFortran['kp'] <= self.Q['kp']

    generator.add('copyQToQFortran', copyQToQFortran)

    stiffnessTensor = Tensor('stiffnessTensor', (3, 3, 3, 3))
    direction = Tensor('direction', (3,))
    christoffel = Tensor('christoffel', (3,3))

    computeChristoffel = christoffel['ik'] <= stiffnessTensor['ijkl'] * direction['j'] * direction['l']
    generator.add('computeChristoffel', computeChristoffel)

  @abstractmethod
  def addLocal(self, generator, targets):
    pass

  @abstractmethod
  def addNeighbor(self, generator, targets):
    pass

  @abstractmethod
  def addTime(self, generator, targets):
    pass

  def add_include_tensors(self, include_tensors):
    include_tensors.add(self.db.samplingDirections)
    include_tensors.add(self.db.M2inv)


class LinearADERDG(ADERDGBase):

  def sourceMatrix(self):
    return None

  def extendedQTensor(self):
    return self.Q

  def numberOfExtendedQuantities(self):
    return self.numberOfQuantities()

  def addInit(self, generator):
    super().addInit(generator)

    iniShape = (self.numberOf3DQuadraturePoints(), self.numberOfQuantities())
    iniCond = OptionalDimTensor('iniCond', self.Q.optName(), self.Q.optSize(), self.Q.optPos(), iniShape, alignStride=True)
    dofsQP = OptionalDimTensor('dofsQP', self.Q.optName(), self.Q.optSize(), self.Q.optPos(), iniShape, alignStride=True)

    generator.add('projectIniCond', self.Q['kp'] <= self.db.projectQP[self.t('kl')] * iniCond['lp'])
    generator.add('evalAtQP', dofsQP['kp'] <= self.db.evalAtQP[self.t('kl')] * self.Q['lp'])

  def addLocal(self, generator, targets):
    for target in targets:
      name_prefix = generate_kernel_name_prefix(target)
      volumeSum = self.Q['kp']
      for i in range(3):
        volumeSum += self.db.kDivM[i][self.t('kl')] * self.I['lq'] * self.starMatrix(i)['qp']
      if self.sourceMatrix():
        volumeSum += self.I['kq'] * self.sourceMatrix()['qp']
      volume = (self.Q['kp'] <= volumeSum)
      generator.add(f'{name_prefix}volume', volume, target=target)

      localFluxNodal = lambda i: self.Q['kp'] <= self.Q['kp'] + self.db.project2nFaceTo3m[i]['kn'] * self.INodal['no'] * self.AminusT['op']
      localFluxNodalPrefetch = lambda i: self.I if i == 0 else (self.Q if i == 1 else None)
      generator.addFamily(f'{name_prefix}localFluxNodal',
                          simpleParameterSpace(4),
                          localFluxNodal,
                          localFluxNodalPrefetch,
                          target=target)

    localFlux = lambda i: self.Q['kp'] <= self.Q['kp'] + self.db.rDivM[i][self.t('km')] * self.db.fMrT[i][self.t('ml')] * self.I['lq'] * self.AplusT['qp']
    localFluxPrefetch = lambda i: self.I if i == 0 else (self.Q if i == 1 else None)
    generator.addFamily(f'localFlux',
                        simpleParameterSpace(4),
                        localFlux,
                        localFluxPrefetch,
                        target='cpu')

    if 'gpu' in targets:
      # Note: quadpy used to generate matricies in SeisSol/matrices
      # requires to a license. Therefore, we use numpy to
      # precompute. rDivM[i] * fMrT[i]
      flux_matrices = dict()
      for i in range(4):
       rDivM = tensor_to_numpy(self.db.rDivM[i])
       fMrT = tensor_to_numpy(self.db.fMrT[i])

       matrix = np.matmul(rDivM, fMrT)
       name = f'fluxPlus({i})'
       flux_matrices[name] = numpy_to_tensor(name=name,
                                             np_array=matrix,
                                             alignStride=True)
      collection = create_collection(flux_matrices)
      self.db.update(collection)

      localFlux = lambda i: self.Q['kp'] <= self.Q['kp'] + self.db.fluxPlus[i]['kl'] * self.I['lq'] * self.AplusT['qp']
      generator.addFamily(f'gpu_localFlux',
                          simpleParameterSpace(4),
                          localFlux,
                          target='gpu')

  def addNeighbor(self, generator, targets):
    neighbourFlux = lambda h, j, i: self.Q['kp'] <= self.Q['kp'] + self.db.rDivM[i][self.t('km')] * self.db.fP[h][self.t('mn')] * self.db.rT[j][self.t('nl')] * self.I['lq'] * self.AminusT['qp']
    neighbourFluxPrefetch = lambda h, j, i: self.I
    generator.addFamily(f'neighboringFlux',
                        simpleParameterSpace(3, 4, 4),
                        neighbourFlux,
                        neighbourFluxPrefetch,
                        target='cpu')

    if 'gpu' in targets:
      # Note: quadpy used to generate matricies in SeisSol/matrices
      # requires to a license. Therefore, we use numpy to
      # precompute. rDivM[i] * fP[h] * rT[j]
      flux_matrices = dict()
      for h in range(3):
        for j in range(4):
          for i in range(4):

            rDivM = tensor_to_numpy(self.db.rDivM[i])
            fP = tensor_to_numpy(self.db.fP[h])
            rT = tensor_to_numpy(self.db.rT[j])

            flux_index = h + 3 * j + 12 * i
            matrix = np.matmul(rDivM, np.matmul(fP, rT))
            name = f'fluxMinus({flux_index})'
            flux_matrices[name] = numpy_to_tensor(name=name,
                                                  np_array=matrix,
                                                  alignStride=True)
      collection = create_collection(flux_matrices)
      self.db.update(collection)

      neighbourFlux = lambda i: self.Q['kp'] <= self.Q['kp'] + self.db.fluxMinus[i]['kl'] * self.I['lq'] * self.AminusT['qp']
      generator.addFamily(f'gpu_neighboringFlux',
                          simpleParameterSpace(48),
                          neighbourFlux,
                          target='gpu')

  def addTime(self, generator, targets):
    for target in targets:
      name_prefix = generate_kernel_name_prefix(target)

      qShape = (self.numberOf3DBasisFunctions(), self.numberOfQuantities())
      dQ0 = OptionalDimTensor('dQ(0)', self.Q.optName(), self.Q.optSize(), self.Q.optPos(), qShape, alignStride=True)
      power = Scalar('power')
      derivatives = [dQ0]
      generator.add(f'{name_prefix}derivativeTaylorExpansion(0)',
                    self.I['kp'] <= power * dQ0['kp'],
                    target=target)

      self.dQs = [dQ0]

      for i in range(1,self.order):
        derivativeSum = Add()
        if self.sourceMatrix():
          derivativeSum += derivatives[-1]['kq'] * self.sourceMatrix()['qp']
        for j in range(3):
          derivativeSum += self.db.kDivMT[j][self.t('kl')] * derivatives[-1]['lq'] * self.starMatrix(j)['qp']

        derivativeSum = DeduceIndices( self.Q['kp'].indices ).visit(derivativeSum)
        derivativeSum = EquivalentSparsityPattern().visit(derivativeSum)
        dQ = OptionalDimTensor('dQ({})'.format(i), self.Q.optName(), self.Q.optSize(), self.Q.optPos(), qShape, spp=derivativeSum.eqspp(), alignStride=True)
        self.dQs.append(dQ)

        generator.add(f'{name_prefix}derivative({i})', dQ['kp'] <= derivativeSum, target=target)
        generator.add(f'{name_prefix}derivativeTaylorExpansion({i})',
                      self.I['kp'] <= self.I['kp'] + power * dQ['kp'],
                      target=target)

        derivatives.append(dQ)

  def add_include_tensors(self, include_tensors):
    super().add_include_tensors(include_tensors)
    include_tensors.add(self.db.nodes2D)

