/// \ingroup base
/// \class ttk::ContourForestsTree
/// \author Charles Gueunet <charles.gueunet@lip6.fr>
/// \date June 2016.
///
///\brief TTK processing package that efficiently computes the contour tree of
/// scalar data and more (data segmentation, topological simplification,
/// persistence diagrams, persistence curves, etc.).
///
///\param dataType Data type of the input scalar field (char, float,
/// etc.).
///
/// \b Related \b publication \n
/// "Contour Forests: Fast Multi-threaded Augmented Contour Trees" \n
/// Charles Gueunet, Pierre Fortin, Julien Jomier, Julien Tierny \n
/// Proc. of IEEE LDAV 2016.
///
/// \sa ttkContourForests.cpp %for a usage example.

#pragma once

#include "ContourForests.h"

namespace ttk {
  namespace cf {

    // ------------------- Contour Forests

    // Process
    // {

    template <typename scalarType, typename triangulationType>
    int ContourForests::build(const triangulationType &mesh) {

#ifdef TTK_ENABLE_OPENMP
      omp_set_num_threads(parallelParams_.nbThreads);
#endif

      DebugTimer timerTOTAL;

      // -----------
      // Paramemters
      // -----------
      initTreeType();
      initNbScalars(mesh);
      initNbPartitions();
      initSoS();

      this->printMsg(std::vector<std::vector<std::string>>{
        {"#Threads", std::to_string(parallelParams_.nbThreads)},
        {"#Partitions", std::to_string(parallelParams_.nbPartitions)}});

      if(params_->simplifyThreshold) {
        this->printMsg(std::vector<std::vector<std::string>>{
          {"Simplify method", std::to_string(params_->simplifyMethod)},
          {"Simplify thresh.", std::to_string(params_->simplifyThreshold)}});
      }

      printDebug(timerTOTAL, "Initialization                   ");

      // ---------
      // Sort Step
      // ---------

      DebugTimer timerSort;
      sortInput<scalarType>();
      printDebug(timerSort, "Sort scalars (+mirror)           ");

      // -------------------
      // Interface & Overlap
      // -------------------

      DebugTimer timerInitOverlap;
      initInterfaces();
      initOverlap(mesh);
      if(params_->debugLevel > 3) {
        for(idInterface i = 0; i < parallelParams_.nbInterfaces; i++) {
          std::cout << "interface : " << static_cast<unsigned>(i);
          std::cout << " seed : " << parallelData_.interfaces[i].getSeed();
          std::cout << std::endl;
        }
      }
      printDebug(timerInitOverlap, "Interface and overlap init.      ");

      // -----------------------
      // Allocate parallel trees
      // -----------------------

      DebugTimer timerAllocPara;
      // Union find std::vector for each partition
      std::vector<std::vector<ExtendedUnionFind *>> vect_baseUF_JT(
        parallelParams_.nbPartitions),

        vect_baseUF_ST(parallelParams_.nbPartitions);
      const SimplexId &resSize
        = (scalars_->size / parallelParams_.nbPartitions) / 10;

      parallelData_.trees.clear();
      parallelData_.trees.reserve(parallelParams_.nbPartitions);

      for(idPartition tree = 0; tree < parallelParams_.nbPartitions; ++tree) {
        // Tree array initialization
        parallelData_.trees.emplace_back(params_, scalars_, tree);
      }

#ifdef TTK_ENABLE_OPENMP
#pragma omp parallel for num_threads(parallelParams_.nbPartitions) \
  schedule(static)
#endif
      for(idPartition tree = 0; tree < parallelParams_.nbPartitions; ++tree) {
        // Tree array initialization
        parallelData_.trees[tree].flush();

        // UF-array reserve
        vect_baseUF_JT[tree].resize(scalars_->size);
        vect_baseUF_ST[tree].resize(scalars_->size);

        // Statistical reserve
        parallelData_.trees[tree].jt_->treeData_.nodes.reserve(resSize);
        parallelData_.trees[tree].jt_->treeData_.superArcs.reserve(resSize);
        parallelData_.trees[tree].st_->treeData_.nodes.reserve(resSize);
        parallelData_.trees[tree].st_->treeData_.superArcs.reserve(resSize);
      }
      printDebug(timerAllocPara, "Parallel allocations             ");

      // -------------------------
      // Build trees in partitions
      // -------------------------

      DebugTimer timerbuild;
      parallelBuild<scalarType>(vect_baseUF_JT, vect_baseUF_ST, mesh);

      if(params_->debugLevel >= 4) {
        if(params_->treeType == TreeType::Contour) {
          for(idPartition i = 0; i < parallelParams_.nbPartitions; i++) {
            std::cout << i << " :" << std::endl;
            parallelData_.trees[i].printTree2();
            std::cout << "-----" << std::endl;
          }
        } else {
          for(idPartition i = 0; i < parallelParams_.nbPartitions; i++) {
            std::cout << i << " jt:" << std::endl;
            parallelData_.trees[i].jt_->printTree2();
            std::cout << i << " st:" << std::endl;
            parallelData_.trees[i].st_->printTree2();
            std::cout << "-----" << std::endl;
          }
        }
      }

      printDebug(timerbuild, "ParallelBuild                    ");

      // --------------------
      // Stitching partitions
      // --------------------

      DebugTimer timerZip;
      if(parallelParams_.partitionNum == -1
         && parallelParams_.nbPartitions > 1) {
        stitch();
        for(idPartition p = 0; p < parallelParams_.nbPartitions; ++p) {

          parallelData_.trees[p].parallelInitNodeValence(
            parallelParams_.nbThreads);
        }
      }

      if(params_->debugLevel >= 4) {
        printVectCT();
      }

      printDebug(timerZip, "Stitch                           ");

      // -------------------------------------------------
      // Unification : create one tree from stitched trees
      // -------------------------------------------------

      DebugTimer timerUnify;
      if(params_->treeType == TreeType::Contour) {
        if(parallelParams_.partitionNum >= 0) {
          if(parallelParams_.partitionNum > parallelParams_.nbInterfaces) {
            clone(&parallelData_.trees[parallelParams_.nbPartitions - 1]);
          } else {
            clone(&parallelData_.trees[parallelParams_.partitionNum]);
          }
        } else if(parallelParams_.nbPartitions == 1) {
          clone(&parallelData_.trees[0]);
        } else {
          unify();
          // for global simlify
          parallelInitNodeValence(parallelParams_.nbThreads);
        }
      } else {
        if(parallelParams_.partitionNum >= 0) {
          if(parallelParams_.partitionNum > parallelParams_.nbInterfaces) {
            jt_->clone(parallelData_.trees[parallelParams_.nbInterfaces].jt_);
            st_->clone(parallelData_.trees[parallelParams_.nbInterfaces].st_);
          } else {
            jt_->clone(parallelData_.trees[parallelParams_.partitionNum].jt_);
            st_->clone(parallelData_.trees[parallelParams_.partitionNum].st_);
          }
        } else if(parallelParams_.nbPartitions == 1) {
          jt_->clone(parallelData_.trees[0].jt_);
          st_->clone(parallelData_.trees[0].st_);
        } else {
          unify();
          jt_->parallelInitNodeValence(parallelParams_.nbThreads);
          st_->parallelInitNodeValence(parallelParams_.nbThreads);
        }
      }

      printDebug(timerUnify, "Contour tree created              ");

      // -------------------
      // Simplification step
      // -------------------

      if(params_->treeType == TreeType::Contour
         && parallelParams_.partitionNum == -1 && params_->simplifyThreshold) {
        DebugTimer timerGlobalSimplify;
        SimplexId simplifed = globalSimplify<scalarType>(-1, nullVertex, mesh);
        if(params_->debugLevel >= 1) {
          printDebug(timerGlobalSimplify, "Simplify Contour tree            ");
          std::cout << " ( " << simplifed << " pairs merged )" << std::endl;
        }
      }

      printDebug(timerTOTAL, "TOTAL                            ");

      // ------------------------------
      // Debug print and memory reclaim
      // ------------------------------

      if(params_->debugLevel >= 5) {
        if(params_->treeType == TreeType::Contour)
          printTree2();
        else {
          std::cout << "JT :" << std::endl;
          jt_->printTree2();
          std::cout << "ST :" << std::endl;
          st_->printTree2();
        }
      } else if(params_->debugLevel > 2) {
        std::stringstream msg;
        if(params_->treeType == TreeType::Contour)
          msg << "max node : " << getNumberOfNodes();
        else {
          msg << "JT max node : " << jt_->getNumberOfNodes();
          msg << "ST max node : " << st_->getNumberOfNodes();
        }
        this->printMsg(msg.str());
      }

      if(params_->treeType == TreeType::Contour) {
        updateSegmentation();
      } else {
        jt_->updateSegmentation();
        st_->updateSegmentation();
      }

      // reclaim memory
      {
        for(idPartition tree = 0; tree < parallelParams_.nbPartitions; ++tree) {
          parallelData_.trees[tree].jt_->treeData_.nodes.shrink_to_fit();
          parallelData_.trees[tree].jt_->treeData_.superArcs.shrink_to_fit();
          parallelData_.trees[tree].st_->treeData_.nodes.shrink_to_fit();
          parallelData_.trees[tree].st_->treeData_.superArcs.shrink_to_fit();
        }
        // Not while arc segmentation depends on std::vector in partitions
        // parallelData_.interfaces.clear();
        // parallelData_.trees.clear();
      }

      return 0;
    }

    template <typename scalarType, typename triangulationType>
    int ContourForests::parallelBuild(
      std::vector<std::vector<ExtendedUnionFind *>> &vect_baseUF_JT,
      std::vector<std::vector<ExtendedUnionFind *>> &vect_baseUF_ST,
      const triangulationType &mesh) {

      std::vector<float> timeSimplify(parallelParams_.nbPartitions, 0);
      std::vector<float> speedProcess(parallelParams_.nbPartitions * 2, 0);
#ifdef TTK_ENABLE_CONTOUR_FORESTS_PARALLEL_SIMPLIFY
      SimplexId nbPairMerged = 0;
#endif

#ifdef TTK_ENABLE_OPENMP
      omp_set_nested(1);
#endif

// std::cout << "NO PARALLEL DEBUG MODE" << std::endl;
#ifdef TTK_ENABLE_OPENMP
#pragma omp parallel for num_threads(parallelParams_.nbPartitions) \
  schedule(static)
#endif
      for(idPartition i = 0; i < parallelParams_.nbPartitions; ++i) {
        DebugTimer timerMergeTree;

        // ------------------------------------------------------
        // Skip partition that are not asked to compute if needed
        // ------------------------------------------------------

        if(parallelParams_.partitionNum != -1
           && parallelParams_.partitionNum != i)
          continue;

        // ------------------------------------------------------
        // Retrieve boundary & overlap list for current partition
        // ------------------------------------------------------

        std::tuple<SimplexId, SimplexId> rangeJT = getJTRange(i);
        std::tuple<SimplexId, SimplexId> rangeST = getSTRange(i);
        std::tuple<SimplexId, SimplexId> seedsPos = getSeedsPos(i);
        std::tuple<std::vector<SimplexId>, std::vector<SimplexId>> overlaps
          = getOverlaps(i);
        const SimplexId &partitionSize
          = abs(std::get<0>(rangeJT) - std::get<1>(rangeJT))
            + std::get<0>(overlaps).size() + std::get<1>(overlaps).size();

        // ---------------
        // Build JT and ST
        // ---------------

#ifdef TTK_ENABLE_OPENMP
#pragma omp parallel sections num_threads(2) if(parallelParams_.lessPartition)
#endif
        {

          // if less partition : we built JT and ST in parallel
#ifdef TTK_ENABLE_OPENMP
#pragma omp section
#endif
          {
            if(params_->treeType == TreeType::Join
               || params_->treeType == TreeType::Contour
               || params_->treeType == TreeType::JoinAndSplit) {
              DebugTimer timerSimplify;
              DebugTimer timerBuild;
              parallelData_.trees[i].getJoinTree()->build(
                vect_baseUF_JT[i], std::get<0>(overlaps), std::get<1>(overlaps),
                std::get<0>(rangeJT), std::get<1>(rangeJT),
                std::get<0>(seedsPos), std::get<1>(seedsPos), mesh);
              speedProcess[i] = partitionSize / timerBuild.getElapsedTime();

#ifdef TTK_ENABLE_CONTOUR_FORESTS_PARALLEL_SIMPLIFY
              timerSimplify.reStart();
              const SimplexId tmpMerge =

                parallelData_.trees[i].getJoinTree()->localSimplify<scalarType>(
                  std::get<0>(seedsPos), std::get<1>(seedsPos));
#ifdef TTK_ENABLE_OPENMP
#pragma omp atomic update
#endif
              timeSimplify[i] += timerSimplify.getElapsedTime();
#ifdef TTK_ENABLE_OPENMP
#pragma omp atomic update
#endif
              nbPairMerged += tmpMerge;
#endif
            }
          }

#ifdef TTK_ENABLE_OPENMP
#pragma omp section
#endif
          {
            if(params_->treeType == TreeType::Split
               || params_->treeType == TreeType::Contour
               || params_->treeType == TreeType::JoinAndSplit) {
              DebugTimer timerSimplify;
              DebugTimer timerBuild;
              parallelData_.trees[i].getSplitTree()->build(
                vect_baseUF_ST[i], std::get<1>(overlaps), std::get<0>(overlaps),
                std::get<0>(rangeST), std::get<1>(rangeST),
                std::get<0>(seedsPos), std::get<1>(seedsPos), mesh);
              speedProcess[parallelParams_.nbPartitions + i]
                = partitionSize / timerBuild.getElapsedTime();

#ifdef TTK_ENABLE_CONTOUR_FORESTS_PARALLEL_SIMPLIFY
              timerSimplify.reStart();
              const SimplexId tmpMerge =

                parallelData_.trees[i]
                  .getSplitTree()
                  ->localSimplify<scalarType>(
                    std::get<0>(seedsPos), std::get<1>(seedsPos));
#ifdef TTK_ENABLE_OPENMP
#pragma omp atomic update
#endif
              timeSimplify[i] += timerSimplify.getElapsedTime();
#ifdef TTK_ENABLE_OPENMP
#pragma omp atomic update
#endif
              nbPairMerged += tmpMerge;
#endif
            }
          }
        }

        this->printMsg("Constructed Merge Tree " + std::to_string(i), 1.0,
                       timerMergeTree.getElapsedTime(), this->threadNumber_);

        // Update segmentation of each arc if needed
        if(params_->simplifyThreshold
           || params_->treeType != TreeType::Contour) {
          DebugTimer timerUpdateSegm;
          parallelData_.trees[i].getJoinTree()->updateSegmentation();
          parallelData_.trees[i].getSplitTree()->updateSegmentation();

          if(params_->debugLevel >= 3) {
            std::cout << "Local MT : updated in "
                      << timerUpdateSegm.getElapsedTime() << std::endl;
          }
        }

        // ---------------
        // Combine JT & ST
        // ---------------

        if(params_->treeType == TreeType::Contour) {
          DebugTimer timerCombine;

          // clone here if we do not want to destry original merge trees!
          auto *jt = parallelData_.trees[i].getJoinTree();
          auto *st = parallelData_.trees[i].getSplitTree();

          // Copy missing nodes of a tree to the other one
          // Maintain this traversal order for good insertion
          for(idNode t = 0; t < st->getNumberOfNodes(); ++t) {
            if(!st->getNode(t)->isHidden()) {
              // std::cout << "insert in jt : " <<
              // st->getNode(t)->getVertexId() << std::endl;
              jt->insertNode(st->getNode(t), true);
            }
          }
          // and vice versa
          for(idNode t = 0; t < jt->getNumberOfNodes(); ++t) {
            if(!jt->getNode(t)->isHidden()) {
              // std::cout << "insert in st : " <<
              // jt->getNode(t)->getVertexId() << std::endl;
              st->insertNode(jt->getNode(t), true);
            }
          }

          // debug print current JT / ST
          if(params_->debugLevel >= 6) {
            std::cout << "Local JT :" << std::endl;
            parallelData_.trees[i].getJoinTree()->printTree2();
            std::cout << "Local ST :" << std::endl;
            parallelData_.trees[i].getSplitTree()->printTree2();
            std::cout << "combine" << std::endl;
          }

          // Combine, destroy JT and ST to compute CT
          parallelData_.trees[i].combine(
            std::get<0>(seedsPos), std::get<1>(seedsPos));
          parallelData_.trees[i].updateSegmentation();

          if(params_->debugLevel > 2) {
            printDebug(timerCombine, "Trees combined   in    ");
          }

          // debug print CT
          if(params_->debugLevel >= 4) {
            parallelData_.trees[i].printTree2();
          }
        } else {
          if(params_->debugLevel >= 6) {
            std::cout << "Local JT :" << std::endl;
            parallelData_.trees[i].getJoinTree()->printTree2();
            std::cout << "Local ST :" << std::endl;
            parallelData_.trees[i].getSplitTree()->printTree2();
            std::cout << "combine" << std::endl;
          }
        }
      } // namespace ttk

      // -------------------------------------
      // Print process speed and simplify info
      // -------------------------------------

      if(params_->debugLevel > 2) {
#ifdef TTK_ENABLE_CONTOUR_FORESTS_PARALLEL_SIMPLIFY
        if(params_->simplifyThreshold) {
          auto maxSimplifIt
            = max_element(timeSimplify.cbegin(), timeSimplify.cend());
          float maxSimplif = *maxSimplifIt;
          std::cout << "Local simplification maximum time :" << maxSimplif;
          std::cout << " ( " << nbPairMerged << " pairs merged )" << std::endl;
        }
#endif
        auto maxProcSpeed
          = max_element(speedProcess.cbegin(), speedProcess.cend());
        auto minProcSpeed
          = min_element(speedProcess.cbegin(), speedProcess.cend());
        std::stringstream msg;
        msg << "process speed : ";
        msg << " min is " << *minProcSpeed << " vert/sec";
        msg << " max is " << *maxProcSpeed << " vert/sec";
        this->printMsg(msg.str());
      }

      return 0;
    }

  } // namespace cf
} // namespace ttk
