#ifndef SLRU_CACHE_HPP
#define SLRU_CACHE_HPP

#include "cache.hpp"

namespace ioremap { namespace cache {

class slru_cache_t {
public:
	slru_cache_t(struct dnet_node *n, const std::vector<size_t> &cache_pages_max_sizes);

	~slru_cache_t();

	void stop();

	int write(const unsigned char *id, dnet_net_state *st, dnet_cmd *cmd, dnet_io_attr *io, const char *data);

	std::shared_ptr<raw_data_t> read(const unsigned char *id, dnet_cmd *cmd, dnet_io_attr *io);

	int remove(const unsigned char *id, dnet_io_attr *io);

	int lookup(const unsigned char *id, dnet_net_state *st, dnet_cmd *cmd);

	cache_stats get_cache_stats() const;

private:
	bool m_need_exit;
	struct dnet_node *m_node;
	size_t m_cache_size, m_max_cache_size;
	std::mutex m_lock;
	size_t m_cache_pages_number;
	std::vector<size_t> m_cache_pages_max_sizes;
	std::vector<size_t> m_cache_pages_sizes;
	std::vector<lru_list_t> m_cache_pages_lru;
	std::thread m_lifecheck;
	treap_t m_treap;
	cache_stats m_cache_stats;

	slru_cache_t(const slru_cache_t &) = delete;

	size_t next_page_number(size_t page_number) const {
		if (page_number == 0) {
			return 0;
		}
		return page_number - 1;
	}

	size_t previous_page_number(size_t page_number) const {
		return page_number + 1;
	}

	void insert_data_into_page(const unsigned char *id, size_t page_number, data_t *data);

	void remove_data_from_page(const unsigned char *id, size_t page_number, data_t *data);

	data_t* create_data(const unsigned char *id, const char *data, size_t size, bool remove_from_disk);

	data_t* populate_from_disk(elliptics_unique_lock<std::mutex> &guard, const unsigned char *id, bool remove_from_disk, int *err);

	void resize_page(const unsigned char *id, size_t page_number, size_t reserve);

	void erase_element(data_t *obj);

	void sync_element(const dnet_id &raw, bool after_append, const std::vector<char> &data, uint64_t user_flags, const dnet_time &timestamp);

	void sync_element(data_t *obj);

	void sync_after_append(elliptics_unique_lock<std::mutex> &guard, bool lock_guard, data_t *obj);

	void life_check(void);
};

}}


#endif // SLRU_CACHE_HPP
