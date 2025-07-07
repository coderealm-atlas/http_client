document.addEventListener('alpine:init', () => {
    console.log("alpine:init called.");
    Alpine.data('pagedata', () => ({
        formData: {
            id: '', url: '', name: '', category: 'github'
        },
        isEditing: false,
        error_msg: '',
        category: '',
        repos: [],
        offset: 0,
        limit: 10,
        // Initialize function to fetch data when the component is created
        init() {
            this._fetch_repos();
            window.cjj365.observable.onChange((prop, oldValue, newValue, source) => {
                if (source !== 'alpinejs') {
                    if (newValue.id === 'save') {
                        this.updateData();
                    }
                    this.formData.value = newValue.value;
                }
            });
        },
        resetForm() {
            this.formData = { id: '', url: '', name: '', category: this.formData.category };
            this.isEditing = false;
        },
        async deleteObj(id) {
            Swal.fire({
                title: "Are you sure?",
                text: "You won't be able to revert this!",
                icon: "warning",
                showCancelButton: true,
                confirmButtonColor: "#3085d6",
                cancelButtonColor: "#d33",
                confirmButtonText: "Yes, delete it!"
            }).then(async (result) => {
                this.deleteData(id);
            });
        },
        editObj(id) {
            const item = this.repos.find(item => item.id === id);
            if (item) {
                this.formData = item;
                this.isEditing = true;
            }
        },
        handlePageChange(action) {
            console.log("Handling back action: " + action);
            if (action == 'forward') {
                this.offset += 10;
            } else {
                this.offset -= 10;
                if (this.offset < 0) {
                    this.offset = 0;
                }
            }
            this.error_msg = "";
            this._fetch_namevalues();
        },
        async deleteData(id) {
            const url = '/admin/remote-repos/' + id;
            try {
                const response = await fetch(url, {
                    method: 'DELETE',
                });
                if (!response.ok) {
                    throw new Error('Network response was not ok');
                }
                const data = await response.json(); //  one
                console.log('Item deleted successfully:', data);
                if (data.data) {
                    this.repos = this.repos.filter(item => item.id != data.data.id);
                    Swal.fire({
                        position: "top-end",
                        title: "Deleted!",
                        text: "Item has been deleted.",
                        icon: "success",
                        showConfirmButton: false,
                        timer: 1500
                    });
                } else {
                    this.showError('Failed to delete the item.');
                }
            } catch (error) {
                console.error('There was an error deleting the item:', error);
                return false;
            }
        },
        showError(text) {
            Swal.fire({
                title: "Failed!",
                text: text ? text : "Something went wrong.",
                icon: "error"
            });
        },
        showSuccess(text) {
            Swal.fire({
                position: "top-end",
                icon: "success",
                title: text ? text : "Your work has been saved",
                showConfirmButton: false,
                timer: 1500
            });
        },
        async postData() {
            try {
                const response = await fetch('/admin/remote-repos', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify(this.formData)
                });

                if (!response.ok) {
                    this.error_msg = `Error: ${response.status}`;
                }
                let data = await response.json(); // Parse JSON response
                this.repos = [data.data, ...this.repos];
                this.isEditing = true;
                return data.data;
            } catch (error) {
                console.error('Error posting data:', error);
                return null; // Or handle the error as needed
            }
        },
        async updateData() {
            try {
                const url = '/admin/remote-repos/' + this.formData.id;
                const response = await fetch(url, {
                    method: 'PUT',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify(this.formData)
                });

                if (!response.ok) {
                    this.error_msg = `Error: ${response.status}`;
                }
                let r = await response.json(); // Parse JSON 
                console.log(r);
                Swal.fire({
                    position: "top-end",
                    icon: "success",
                    title: "Your work has been saved",
                    showConfirmButton: false,
                    timer: 1500
                });
            } catch (error) {
                console.error('Error posting data:', error);
                return null; // Or handle the error as needed
            }
        },
        async fetchUpstream(id) {
            try {
                const url = '/admin/remote-repos/' + id + "/fetch";
                const response = await fetch(url, {
                    method: 'GET',
                    headers: {
                        'Content-Type': 'application/json'
                    }
                });
                if (!response.ok) {
                    this.showError("Response is not Ok.");
                } else {
                    let data = await response.json(); // Parse JSON 
                    console.log(data);
                    if (data.status === 'error') {
                        this.showError('Fetch or Clone repo failed.');
                    } else {
                        this.showSuccess();
                    }
                }
            } catch (error) {
                console.error('Error posting data:', error);
                return null; // Or handle the error as needed
            }
        },
        async _fetch_repos() {
            try {
                const url = new URL("/admin/remote-repos", window.location.origin);
                const params = new URLSearchParams(url.search);
                params.set('offset', this.offset);
                params.set('limit', this.limit);
                params.set('category', this.category);
                const newUrl = `${url.pathname}?${params.toString()}`;
                console.log("Constructed URL:", newUrl);
                const response = await fetch(newUrl, {
                    method: 'GET',
                    credentials: 'include',
                    headers: {
                        "Accept": "application/json" // Specify that we want JSON
                    }
                });
                const data = await response.json();
                console.log(data);
                this.repos = data.data || []
                //return data; // Return the data after it has been fetched
            } catch (error) {
                console.error('Error fetching data:', error);
                return null; // In case of an error, return null
            }
        }
    }));
});
<html><head></head><body><li><p class="hhhiden">hide</p></li></body></html>