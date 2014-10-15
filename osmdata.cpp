#include "osmdata.hpp"

#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>
#include <boost/thread/thread.hpp>
#include <boost/unordered_map.hpp>
#include <boost/thread.hpp>
#include <boost/version.hpp>

#include <stdexcept>

#if BOOST_VERSION < 105300
#else
#include <boost/atomic.hpp>
#endif

osmdata_t::osmdata_t(boost::shared_ptr<middle_t> mid_, const boost::shared_ptr<output_t>& out_): mid(mid_)
{
    outs.push_back(out_);
}

osmdata_t::osmdata_t(boost::shared_ptr<middle_t> mid_, const std::vector<boost::shared_ptr<output_t> > &outs_)
    : mid(mid_), outs(outs_)
{
    if (outs.empty()) {
        throw std::runtime_error("Must have at least one output, but none have "
                                 "been configured.");
    }
}

osmdata_t::~osmdata_t()
{
}

int osmdata_t::node_add(osmid_t id, double lat, double lon, struct keyval *tags) {
    mid->nodes_set(id, lat, lon, tags);

    int status = 0;
    BOOST_FOREACH(boost::shared_ptr<output_t>& out, outs) {
        status |= out->node_add(id, lat, lon, tags);
    }
    return status;
}

int osmdata_t::way_add(osmid_t id, osmid_t *nodes, int node_count, struct keyval *tags) {
    mid->ways_set(id, nodes, node_count, tags);
    
    int status = 0;
    BOOST_FOREACH(boost::shared_ptr<output_t>& out, outs) {
        status |= out->way_add(id, nodes, node_count, tags);
    }
    return status;
}

int osmdata_t::relation_add(osmid_t id, struct member *members, int member_count, struct keyval *tags) {
    mid->relations_set(id, members, member_count, tags);
    
    int status = 0;
    BOOST_FOREACH(boost::shared_ptr<output_t>& out, outs) {
        status |= out->relation_add(id, members, member_count, tags);
    }
    return status;
}

int osmdata_t::node_modify(osmid_t id, double lat, double lon, struct keyval *tags) {
    slim_middle_t *slim = dynamic_cast<slim_middle_t *>(mid.get());

    slim->nodes_delete(id);
    slim->nodes_set(id, lat, lon, tags);

    int status = 0;
    BOOST_FOREACH(boost::shared_ptr<output_t>& out, outs) {
        status |= out->node_modify(id, lat, lon, tags);
    }

    slim->node_changed(id);

    return status;
}

int osmdata_t::way_modify(osmid_t id, osmid_t *nodes, int node_count, struct keyval *tags) {
    slim_middle_t *slim = dynamic_cast<slim_middle_t *>(mid.get());

    slim->ways_delete(id);
    slim->ways_set(id, nodes, node_count, tags);

    int status = 0;
    BOOST_FOREACH(boost::shared_ptr<output_t>& out, outs) {
        status |= out->way_modify(id, nodes, node_count, tags);
    }

    slim->way_changed(id);

    return status;
}

int osmdata_t::relation_modify(osmid_t id, struct member *members, int member_count, struct keyval *tags) {
    slim_middle_t *slim = dynamic_cast<slim_middle_t *>(mid.get());

    slim->relations_delete(id);
    slim->relations_set(id, members, member_count, tags);

    int status = 0;
    BOOST_FOREACH(boost::shared_ptr<output_t>& out, outs) {
        status |= out->relation_modify(id, members, member_count, tags);
    }

    slim->relation_changed(id);

    return status;
}

int osmdata_t::node_delete(osmid_t id) {
    slim_middle_t *slim = dynamic_cast<slim_middle_t *>(mid.get());

    int status = 0;
    BOOST_FOREACH(boost::shared_ptr<output_t>& out, outs) {
        status |= out->node_delete(id);
    }

    slim->nodes_delete(id);

    return status;
}

int osmdata_t::way_delete(osmid_t id) {
    slim_middle_t *slim = dynamic_cast<slim_middle_t *>(mid.get());

    int status = 0;
    BOOST_FOREACH(boost::shared_ptr<output_t>& out, outs) {
        status |= out->way_delete(id);
    }

    slim->ways_delete(id);

    return status;
}

int osmdata_t::relation_delete(osmid_t id) {
    slim_middle_t *slim = dynamic_cast<slim_middle_t *>(mid.get());

    int status = 0;
    BOOST_FOREACH(boost::shared_ptr<output_t>& out, outs) {
        status |= out->relation_delete(id);
    }

    slim->relations_delete(id);

    return status;
}

void osmdata_t::start() {
    BOOST_FOREACH(boost::shared_ptr<output_t>& out, outs) {
        out->start();
    }
    mid->start(outs[0]->get_options());
}

namespace {

//TODO: have the main thread using the main middle to query the middle for batches of ways (configurable number)
//and stuffing those into the work queue, so we have a single producer multi consumer threaded queue
//since the fetching from middle should be faster than the processing in each backend.

struct pending_threaded_processor : public middle_t::pending_processor {
    typedef std::vector<boost::shared_ptr<output_t> > output_vec_t;
    typedef std::pair<boost::shared_ptr<const middle_query_t>, output_vec_t> clone_t;

#if BOOST_VERSION < 105300
    static void do_jobs(output_vec_t const& outputs, pending_queue_t& queue, size_t& ids_done, boost::mutex& mutex, int append, bool ways) {
        while (true) {
            //get the job off the queue synchronously
            pending_job_t job;
            mutex.lock();
            if(queue.empty()) {
                mutex.unlock();
                break;
            }
            else {
                job = queue.top();
                queue.pop();
            }
            mutex.unlock();

            //process it
            if(ways)
                outputs.at(job.second)->pending_way(job.first, append);
            else
                outputs.at(job.second)->pending_relation(job.first, append);

            mutex.lock();
            ++ids_done;
            mutex.unlock();
        }
    }
#else
    static void do_jobs(output_vec_t const& outputs, pending_queue_t& queue, boost::atomic_size_t& ids_done, int append, bool ways) {
        pending_job_t job;
        while (queue.pop(job)) {
            if(ways)
                outputs.at(job.second)->pending_way(job.first, append);
            else
                outputs.at(job.second)->pending_relation(job.first, append);
            ++ids_done;
        }
    }
#endif

    //starts up count threads and works on the queue
    pending_threaded_processor(boost::shared_ptr<middle_query_t> mid, const output_vec_t& outs, size_t thread_count, size_t job_count, int append)
#if BOOST_VERSION < 105300
        //note that we cant hint to the stack how large it should be ahead of time
        //we could use a different datastructure like a deque or vector but then
        //the outputs the enqueue jobs would need the version check for the push(_back) method
        : outs(outs), ids_queued(0), append(append), queue(), ids_done(0) {
#else
        : outs(outs), ids_queued(0), append(append), queue(job_count), ids_done(0) {
#endif

        //clone all the things we need
        clones.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i) {
            //clone the middle
            boost::shared_ptr<const middle_query_t> mid_clone = mid->get_instance();

            //clone the outs
            output_vec_t out_clones;
            BOOST_FOREACH(const boost::shared_ptr<output_t>& out, outs) {
                out_clones.push_back(out->clone(mid_clone.get()));
            }

            //keep the clones for a specific thread to use
            clones.push_back(clone_t(mid_clone, out_clones));
        }
    }

    ~pending_threaded_processor() {}

    void enqueue_ways(osmid_t id) {
        for(size_t i = 0; i < outs.size(); ++i) {
            outs[i]->enqueue_ways(queue, id, i, ids_queued);
        }
    }

    //waits for the completion of all outstanding jobs
    void process_ways() {
        //reset the number we've done
        ids_done = 0;

        fprintf(stderr, "\nGoing over pending ways...\n");
        fprintf(stderr, "\t%zu ways are pending\n", ids_queued);
        fprintf(stderr, "\nUsing %zu helper-processes\n", clones.size());
        time_t start = time(NULL);


        //make the threads and start them
        for (size_t i = 0; i < clones.size(); ++i) {
#if BOOST_VERSION < 105300
            workers.create_thread(boost::bind(do_jobs, boost::cref(clones[i].second), boost::ref(queue), boost::ref(ids_done), boost::ref(mutex), append, true));
#else
            workers.create_thread(boost::bind(do_jobs, boost::cref(clones[i].second), boost::ref(queue), boost::ref(ids_done), append, true));
#endif
        }

        //TODO: print out partial progress

        //wait for them to really be done
        workers.join_all();

        time_t finish = time(NULL);
        fprintf(stderr, "\rFinished processing %zu ways in %i sec\n\n", ids_queued, (int)(finish - start));
        if (finish - start > 0)
            fprintf(stderr, "%zu Pending ways took %ds at a rate of %.2f/s\n", ids_queued, (int)(finish - start),
                    ((double)ids_queued / (double)(finish - start)));
        ids_queued = 0;
        ids_done = 0;

        //collect all the new rels that became pending from each
        //output in each thread back to their respective main outputs
        BOOST_FOREACH(const clone_t& clone, clones) {
            //for each clone/original output
            for(output_vec_t::const_iterator original_output = outs.begin(), clone_output = clone.second.begin();
                original_output != outs.end() && clone_output != clone.second.end(); ++original_output, ++clone_output) {
                //done copying ways for now
                clone_output->get()->commit();
                //merge the pending from this threads copy of output back
                original_output->get()->merge_pending_relations(*clone_output);
            }
        }
    }

    void enqueue_relations(osmid_t id) {
        for(size_t i = 0; i < outs.size(); ++i) {
            outs[i]->enqueue_relations(queue, id, i, ids_queued);
        }
    }

    void process_relations() {
        //reset the number we've done
        ids_done = 0;

        fprintf(stderr, "\nGoing over pending relations...\n");
        fprintf(stderr, "\t%zu relations are pending\n", ids_queued);
        fprintf(stderr, "\nUsing %zu helper-processes\n", clones.size());
        time_t start = time(NULL);

        //make the threads and start them
        for (size_t i = 0; i < clones.size(); ++i) {
#if BOOST_VERSION < 105300
            workers.create_thread(boost::bind(do_jobs, boost::cref(clones[i].second), boost::ref(queue), boost::ref(ids_done), boost::ref(mutex), append, false));
#else
            workers.create_thread(boost::bind(do_jobs, boost::cref(clones[i].second), boost::ref(queue), boost::ref(ids_done), append, false));
#endif
        }

        //TODO: print out partial progress

        //wait for them to really be done
        workers.join_all();

        time_t finish = time(NULL);
        fprintf(stderr, "\rFinished processing %zu relations in %i sec\n\n", ids_queued, (int)(finish - start));
        if (finish - start > 0)
            fprintf(stderr, "%zu Pending relations took %ds at a rate of %.2f/s\n", ids_queued, (int)(finish - start),
                    ((double)ids_queued / (double)(finish - start)));
        ids_queued = 0;
        ids_done = 0;

        //collect all expiry tree informations together into one
        BOOST_FOREACH(const clone_t& clone, clones) {
            //for each clone/original output
            for(output_vec_t::const_iterator original_output = outs.begin(), clone_output = clone.second.begin();
                original_output != outs.end() && clone_output != clone.second.end(); ++original_output, ++clone_output) {
                //done copying rels for now
                clone_output->get()->commit();
                //merge the expire tree from this threads copy of output back
                original_output->get()->merge_expire_trees(*clone_output);
            }
        }
    }

private:
    //middle and output copies
    std::vector<clone_t> clones;
    output_vec_t outs; //would like to move ownership of outs to osmdata_t and middle passed to output_t instead of owned by it
    //actual threads
    boost::thread_group workers;
    //how many jobs do we have in the queue to start with
    size_t ids_queued;
    //appending to output that is already there (diff processing)
    int append;
    //job queue
    pending_queue_t queue;

#if BOOST_VERSION < 105300
    //how many ids within the job have been processed
    size_t ids_done;
    //so the threads can manage some of the shared state
    boost::mutex mutex;
#else
    boost::atomic_size_t ids_done;
#endif
};

} // anonymous namespace

void osmdata_t::stop() {
    /* Commit the transactions, so that multiple processes can
     * access the data simultanious to process the rest in parallel
     * as well as see the newly created tables.
     */
    size_t pending_count = mid->pending_count();
    mid->commit();
    BOOST_FOREACH(boost::shared_ptr<output_t>& out, outs) {
        //TODO: each of the outs can be in parallel
        out->commit();
        pending_count += out->pending_count();
    }

    // should be the same for all outputs
    const int append = outs[0]->get_options()->append;

    //threaded pending processing
    pending_threaded_processor ptp(mid, outs, outs[0]->get_options()->num_procs, pending_count, append);

    if (!outs.empty()) {
        //This stage takes ways which were processed earlier, but might be
        //involved in a multipolygon relation. They could also be ways that
        //were modified in diff processing.
        mid->iterate_ways( ptp );

        //This is like pending ways, except there aren't pending relations
        //on import, only on update.
        //TODO: Can we skip this on import?
        mid->iterate_relations( ptp );
    }

	//Clustering, index creation, and cleanup.
	//All the intensive parts of this are long-running PostgreSQL commands
    boost::thread_group threads;
    BOOST_FOREACH(boost::shared_ptr<output_t>& out, outs) {
        threads.add_thread(new boost::thread(boost::bind( &output_t::stop, out.get() )));
    }
    threads.add_thread(new boost::thread(boost::bind( &middle_t::stop, mid.get() )));
    threads.join_all();
}
