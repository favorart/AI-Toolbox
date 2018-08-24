#ifndef AI_TOOLBOX_UTILS_POLYTOPE_HEADER_FILE
#define AI_TOOLBOX_UTILS_POLYTOPE_HEADER_FILE

#include <AIToolbox/Utils/Combinatorics.hpp>
#include <Eigen/Dense>

#include <AIToolbox/LP.hpp>

namespace AIToolbox {
    /**
     * @brief This function implements a naive vertex enumeration algorithm.
     *
     * This function goes through every subset of planes of size S, and finds
     * all vertices it can. In particular, it goes through the first list one
     * element at a time, and joins it with S-1 elements from the second list.
     *
     * Even more precisely, we take >= 1 elements from the second list. The
     * remaining elements (so that in total we still use S-1) are simply the
     * simplex boundaries, which allows us to find the corners located there.
     *
     * This method may find duplicate vertices (it does not bother to prune
     * them), as a vertex can be in the convergence of more than S planes.
     *
     * The advantage is that we do not need any linear programming, and simple
     * matrix decomposition techniques suffice.
     *
     * Warning: the values of each vertex depends on the planes it has been
     * found of, and thus may *not* be the true value if considering all planes
     * at the same time!
     *
     * This function works on ranges of Vectors.
     *
     * @param beginNew The beginning of the range of the planes to find vertices for.
     * @param endNew The end of the range of the planes to find vertices for.
     * @param alphasBegin The beginning of the range of all other planes.
     * @param alphasEnd The end of the range of all other planes.
     *
     * @return A non-unique list of all the vertices found.
     */
    template <typename NewIt, typename OldIt>
    std::vector<std::pair<Vector, double>> findVerticesNaive(NewIt beginNew, NewIt endNew, OldIt alphasBegin, OldIt alphasEnd) {
        std::vector<std::pair<Vector, double>> vertices;

        const size_t alphasSize = std::distance(alphasBegin, alphasEnd);
        if (alphasSize == 0) return vertices;
        const size_t S = alphasBegin->size();

        // This enumerator allows us to compute all possible subsets of S-1
        // elements. We use it on both the alphas, and the boundaries, thus the
        // number of elements we iterate over is alphasSize + S.
        SubsetEnumerator enumerator(S - 1, 0ul, alphasSize + S);

        // This is the matrix on the left side of Ax = b (where A is m)
        Matrix2D m(S + 1, S + 1);
        m.row(0)[S] = -1; // First row is always a vector

        Vector boundary(S+1);
        boundary[S] = 0.0; // The boundary doesn't care about the value

        // This is the vector on the right side of Ax = b
        Vector b(S+1); b.fill(0.0);

        Vector result(S+1);

        // Common matrix/vector setups

        for (auto newVIt = beginNew; newVIt != endNew; ++newVIt) {
            m.row(0).head(S) = *newVIt;

            enumerator.reset();

            // Get subset of planes, find corner with LU
            size_t last = 0;
            while (enumerator.isValid()) {
                // Reset boundaries to care about all dimensions
                boundary.head(S).fill(1.0);
                size_t counter = 1;
                // Note that we start from last to avoid re-copying vectors
                // that are already in the matrix in their correct place.
                for (auto i = last; i < enumerator->size(); ++i) {
                    // For each value in the enumerator, if it is less than
                    // alphasSize it is referring to an alphaVector we need to
                    // take into account.
                    const auto index = (*enumerator)[i];
                    if (index < alphasSize) {
                        // Copy the right vector in the matrix.
                        m.row(counter).head(S) = *std::next(alphasBegin, index);
                        m.row(counter)[S] = -1;
                        ++counter;
                    } else {
                        // We limit the index-th dimension (minus alphasSize to scale in a 0-S range)
                        boundary[index - alphasSize] = 0.0;
                    }
                }
                m.row(counter) = boundary;
                b[counter] = 1.0;
                ++counter;

                // Note that we only need to consider the first "counter" rows,
                // as the boundaries get merged in a single one.
                result = m.topRows(counter).colPivHouseholderQr().solve(b.head(counter));

                b[counter-1] = 0.0;

                // Add to found only if valid, otherwise skip.
                if (((result.head(S).array() >= 0) && (result.head(S).array() <= 1.0)).all())
                    vertices.emplace_back(result.head(S), result[S]);

                // Advance, and take the id of the first index changed in the
                // next combination.
                last = enumerator.advance();

                // If the index went over the alpha list, then we'd only have
                // boundaries, but we don't care about those cases (since we
                // assume we already have the corners of the simplex computed).
                // Thus, terminate.
                if ((*enumerator)[last] >= alphasSize) break;
            }
        }
        return vertices;
    }

    /**
     * @brief This function computes the optimistic value of a point given known vertices and values.
     *
     * This function computes an LP to determine the best possible value of a
     * point given all known best vertices around it.
     *
     * This function is needed in multi-objective settings (rather than
     * POMDPs), since the step where we compute the optimal value for a given
     * point is extremely expensive (it requires solving a full MDP). Thus
     * linear programming is used in order to determine an optimistic bound
     * when deciding the next point to extract from the queue during the linear
     * support process.
     *
     * @param b The point where we want to compute the best possible value.
     * @param bvBegin The start of the range of point-value pairs representing all surrounding vertices.
     * @param bvEnd The end of that same range.
     *
     * @return The best possible value that the input point can have given the known vertices.
     */
    template <typename It>
    double computeOptimisticValue(const Vector & p, It pvBegin, It pvEnd) {
        const size_t vertexNumber = std::distance(pvBegin, pvEnd);
        if (vertexNumber == 0) return 0.0;
        const size_t S = p.size();

        LP lp(S);

        /*
         * With this LP we are looking for an optimistic hyperplane that can
         * tightly fit all corners that we already have, and maximize the value
         * at the input point.
         *
         * Our constraints are of the form
         *
         * vertex[0][0]) * h0 + vertex[0][1]) * h1 + ... <= vertex[0].currentValue
         * vertex[1][0]) * h0 + vertex[1][1]) * h1 + ... <= vertex[1].currentValue
         * ...
         *
         * Since we are looking for an optimistic hyperplane, all variables are
         * unbounded since the hyperplane may need to go negative at some
         * states.
         *
         * Finally, our constraint is a row to maximize:
         *
         * b * v0 + b * v1 + ...
         *
         * Which means we try to maximize the value of the input point with the
         * newly found hyperplane.
         */

        // Set objective to maximize
        lp.row = p;
        lp.setObjective(true);

        // Set unconstrained to all variables
        for (size_t s = 0; s < S; ++s)
            lp.setUnbounded(s);

        // Set constraints for all input belief points and current values.
        for (auto it = pvBegin; it != pvEnd; ++it) {
            lp.row = it->first;
            lp.pushRow(LP::Constraint::LessEqual, it->second);
        }

        double retval;
        // Note that we don't care about the optimistic alphavector, so we
        // discard it. We check that everything went fine though, in theory
        // there shouldn't be any problems here.
        auto solution = lp.solve(0, &retval);
        assert(solution);

        return retval;
    }
}

#endif