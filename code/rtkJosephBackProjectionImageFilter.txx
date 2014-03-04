/*=========================================================================
 *
 *  Copyright RTK Consortium
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0.txt
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *=========================================================================*/

#ifndef __rtkJosephBackProjectionImageFilter_txx
#define __rtkJosephBackProjectionImageFilter_txx

#include "rtkHomogeneousMatrix.h"
#include "rtkRayBoxIntersectionFunction.h"

#include <itkImageRegionConstIterator.h>
#include <itkImageRegionIteratorWithIndex.h>
#include <itkIdentityTransform.h>

namespace rtk
{

template <class TInputImage,
          class TOutputImage,
          class TSplatWeightMultiplication>
void
JosephBackProjectionImageFilter<TInputImage,
                                   TOutputImage,
                                   TSplatWeightMultiplication>
::GenerateData()
{
  const unsigned int Dimension = TInputImage::ImageDimension;
  typename TInputImage::RegionType Largest =this->GetInput(1)->GetLargestPossibleRegion(); 
  const unsigned int nPixelPerProj = Largest.GetSize(0) * Largest.GetSize(1);
  int offsets[3];
  offsets[0] = 1;
  offsets[1] = this->GetInput(0)->GetBufferedRegion().GetSize()[0];
  offsets[2] = this->GetInput(0)->GetBufferedRegion().GetSize()[0] * this->GetInput(0)->GetBufferedRegion().GetSize()[1];

//  const typename JosephBackProjectionImageFilter<TInputImage,
//                                 TOutputImage,
//                                 TSplatWeightMultiplication>
//::
  GeometryType *geometry = dynamic_cast<GeometryType*>(this->GetGeometry().GetPointer());

  // Allocate the output image
  this->AllocateOutputs();

  // beginBuffer is pointing at point with index (0,0,0) in memory, even if
  // it is not in the allocated memory
  typename TOutputImage::PixelType *beginBuffer =
      this->GetOutput()->GetBufferPointer() -
      offsets[0] * this->GetOutput()->GetBufferedRegion().GetIndex()[0] -
      offsets[1] * this->GetOutput()->GetBufferedRegion().GetIndex()[1] -
      offsets[2] * this->GetOutput()->GetBufferedRegion().GetIndex()[2];

  // Iterator on projections input
  typedef itk::ImageRegionConstIterator<TInputImage> InputRegionIterator;
  InputRegionIterator itIn(this->GetInput(1), Largest);

  // Create intersection functions, one for each possible main direction
  typedef rtk::RayBoxIntersectionFunction<CoordRepType, Dimension> RBIFunctionType;
  typename RBIFunctionType::Pointer rbi[Dimension];
  for(unsigned int j=0; j<Dimension; j++)
    {
    rbi[j] = RBIFunctionType::New();
    typename RBIFunctionType::VectorType boxMin, boxMax;
    for(unsigned int i=0; i<Dimension; i++)
      {
      boxMin[i] = this->GetOutput()->GetBufferedRegion().GetIndex()[i] + 0.001;  // To avoid numerical errors
      boxMax[i] = this->GetOutput()->GetBufferedRegion().GetIndex()[i] +
                  this->GetOutput()->GetBufferedRegion().GetSize()[i]  - 1.001;  // To avoid numerical errors
      }
    rbi[j]->SetBoxMin(boxMin);
    rbi[j]->SetBoxMax(boxMax);
    }

  // Go over each projection
  for(int iProj=Largest.GetIndex(2);
          iProj<Largest.GetIndex(2)+(int)Largest.GetSize(2);
          iProj++)
    {
    // Account for system rotations
    // volPPToIndex maps the physical 3D coordinates of a point (in mm) to the
    // corresponding 3D volume index
    typename GeometryType::ThreeDHomogeneousMatrixType volPPToIndex;
    volPPToIndex = GetPhysicalPointToIndexMatrix( this->GetOutput() );

    // Set source position in volume indices
    // GetSourcePosition() returns coordinates in mm. Multiplying by 
    // volPPToIndex gives the corresponding volume index
    typename GeometryType::HomogeneousVectorType sourcePosition;
    sourcePosition = volPPToIndex * geometry->GetSourcePosition(iProj);
    for(unsigned int i=0; i<Dimension; i++)
      rbi[i]->SetRayOrigin( &(sourcePosition[0]) );

    // Compute matrix to transform projection index to volume index
    // IndexToPhysicalPointMatrix maps the 2D index of a projection's pixel to its 2D position on the detector (in mm)
    // ProjectionCoordinatesToFixedSystemMatrix maps the 2D position of a pixel on the detector to its 3D coordinates in volume's coordinates (still in mm)
    // volPPToIndex maps 3D volume coordinates to a 3D index
    typename GeometryType::ThreeDHomogeneousMatrixType matrix;
    matrix = volPPToIndex.GetVnlMatrix() *
             geometry->GetProjectionCoordinatesToFixedSystemMatrix(iProj).GetVnlMatrix() *
             GetIndexToPhysicalPointMatrix( this->GetInput(1) ).GetVnlMatrix();

    // Go over each pixel of the projection
    typename RBIFunctionType::VectorType dirVox, stepMM, dirVoxAbs, np, fp;
    for(unsigned int pix=0; pix<nPixelPerProj; pix++, ++itIn)
      {
      // Compute point coordinate in volume depending on projection index
      for(unsigned int i=0; i<Dimension; i++)
        {
        dirVox[i] = matrix[i][Dimension];
        for(unsigned int j=0; j<Dimension; j++)
          dirVox[i] += matrix[i][j] * itIn.GetIndex()[j];

        // Direction
        dirVox[i] -= sourcePosition[i];
        }

      // Select main direction
      unsigned int mainDir = 0;
      for(unsigned int i=0; i<Dimension; i++)
        {
        dirVoxAbs[i] = vnl_math_abs( dirVox[i] );
        if(dirVoxAbs[i]>dirVoxAbs[mainDir])
          mainDir = i;
        }

      // Test if there is an intersection
      if( rbi[mainDir]->Evaluate(dirVox) &&
          rbi[mainDir]->GetFarthestDistance()>=0. && // check if detector after the source
          rbi[mainDir]->GetNearestDistance()<=1.)    // check if detector after or in the volume
        {
        // Clip the casting between source and pixel of the detector
        rbi[mainDir]->SetNearestDistance ( std::max(rbi[mainDir]->GetNearestDistance() , 0.) );
        rbi[mainDir]->SetFarthestDistance( std::min(rbi[mainDir]->GetFarthestDistance(), 1.) );

        // Compute and sort intersections: (n)earest and (f)arthest (p)points
        np = rbi[mainDir]->GetNearestPoint();
        fp = rbi[mainDir]->GetFarthestPoint();
        if(np[mainDir]>fp[mainDir])
          std::swap(np, fp);

        // Compute main nearest and farthest slice indices
        const int ns = vnl_math_ceil ( np[mainDir] );
        const int fs = vnl_math_floor( fp[mainDir] );

        // If its a corner, we can skip
        if( fs<ns )
          {
        //  itOut.Set( m_ProjectedValueAccumulation(threadId,
        //                                          itIn.Get(),
        //                                          0.,
        //                                          &(sourcePosition[0]),
        //                                          &(sourcePosition[0]),
        //                                          dirVox,
        //                                          &(sourcePosition[0]),
        //                                          &(sourcePosition[0])) );
           continue;
           }

        // Determine the other two directions
        unsigned int notMainDirInf = (mainDir+1)%Dimension;
        unsigned int notMainDirSup = (mainDir+2)%Dimension;
        if(notMainDirInf>notMainDirSup)
          std::swap(notMainDirInf, notMainDirSup);

        // Init data pointers to first pixel of slice ns (i)nferior and (s)uperior (x|y) corner
        const int offsetx = offsets[notMainDirInf];
        const int offsety = offsets[notMainDirSup];
        const int offsetz = offsets[mainDir];
        OutputPixelType *pxiyi, *pxsyi, *pxiys, *pxsys;

        pxiyi = beginBuffer + ns * offsetz;
        pxsyi = pxiyi + offsetx;
        pxiys = pxiyi + offsety;
        pxsys = pxsyi + offsety;

        // Compute step size and go to first voxel
        const CoordRepType residual = ns-np[mainDir];
        const CoordRepType norm = 1/dirVox[mainDir];
        const CoordRepType stepx = dirVox[notMainDirInf] * norm;
        const CoordRepType stepy = dirVox[notMainDirSup] * norm;
        CoordRepType currentx = np[notMainDirInf] + residual*stepx;
        CoordRepType currenty = np[notMainDirSup] + residual*stepy;

        // Compute voxel to millimeters conversion
        stepMM[notMainDirInf] = this->GetInput(0)->GetSpacing()[notMainDirInf] * stepx;
        stepMM[notMainDirSup] = this->GetInput(0)->GetSpacing()[notMainDirSup] * stepy;
        stepMM[mainDir]       = this->GetInput(0)->GetSpacing()[mainDir];

        // First step
        BilinearSplat(itIn.Get(), residual + 0.5, stepMM.GetNorm(), pxiyi, pxsyi, pxiys, pxsys, currentx, currenty, offsetx, offsety);
        
        // Middle steps
        for(int i=ns; i<fs-1; i++)
          {
          pxiyi += offsetz;
          pxsyi += offsetz;
          pxiys += offsetz;
          pxsys += offsetz;
          currentx += stepx;
          currenty += stepy;

          BilinearSplat(itIn.Get(), 1.0, stepMM.GetNorm(), pxiyi, pxsyi, pxiys, pxsys, currentx, currenty, offsetx, offsety);
          }

        // Last step: goes to next voxel only if more than one
        if(ns!=fs)
          {
          pxiyi += offsetz;
          pxsyi += offsetz;
          pxiys += offsetz;
          pxsys += offsetz;
          currentx += stepx;
          currenty += stepy;
          }

          BilinearSplat(itIn.Get(), fp[mainDir]-fs +0.5, stepMM.GetNorm(), pxiyi, pxsyi, pxiys, pxsys, currentx, currenty, offsetx, offsety); 

        }

      }
    }
}

template <class TInputImage,
          class TOutputImage,
          class TSplatWeightMultiplication>
void
JosephBackProjectionImageFilter<TInputImage,
                                   TOutputImage,
                                   TSplatWeightMultiplication>
::BilinearSplat(const InputPixelType rayValue,
                                               const double stepLengthInVoxel,
                                               const double voxelSize,
                                               OutputPixelType *pxiyi,
                                               OutputPixelType *pxsyi,
                                               OutputPixelType *pxiys,
                                               OutputPixelType *pxsys,
                                               const double x,
                                               const double y,
                                               const int ox,
                                               const int oy)
{
  int ix = vnl_math_floor(x);
  int iy = vnl_math_floor(y);
  int idx = ix*ox + iy*oy;
  CoordRepType lx = x - ix;
  CoordRepType ly = y - iy;
  CoordRepType lxc = 1.-lx;
  CoordRepType lyc = 1.-ly;

  pxiyi[idx] += m_SplatWeightMultiplication(rayValue, stepLengthInVoxel, voxelSize, lxc * lyc);
  pxsyi[idx] += m_SplatWeightMultiplication(rayValue, stepLengthInVoxel, voxelSize, lx * lyc);
  pxiys[idx] += m_SplatWeightMultiplication(rayValue, stepLengthInVoxel, voxelSize, lxc * ly);
  pxsys[idx] += m_SplatWeightMultiplication(rayValue, stepLengthInVoxel, voxelSize, lx * ly);

}

} // end namespace rtk

#endif